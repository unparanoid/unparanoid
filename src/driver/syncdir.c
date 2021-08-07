#include "common.h"


#define LOG_PREFIX_ "upd.syncdir: "

#define DEFAULT_PERMISSION_ 0600

#define RULE_BACKEND_STACK_MAX_ 8


typedef struct ctx_t_       ctx_t_;
typedef struct rule_t_      rule_t_;
typedef struct rule_item_t_ rule_item_t_;

struct ctx_t_ {
  upd_file_t*      parent;
  upd_file_watch_t watch;

  upd_array_of(rule_t_*) rules;
  upd_array_of(upd_req_dir_entity_t*) children;

  upd_file_lock_t          lock;
  upd_array_of(upd_req_t*) reqs;

  unsigned selflock : 1;
  unsigned busy     : 1;
};

struct rule_t_ {
  uint8_t* pattern;

  upd_array_of(rule_item_t_*) items;
};

struct rule_item_t_ {
  uint8_t* param;
  size_t   paramlen;

  const upd_driver_t* driver;
};


static
bool
syncdir_parse_param_(
  upd_file_t* f);

static
bool
syncdir_init_(
  upd_file_t* f);

static
void
syncdir_deinit_(
  upd_file_t* f);

static
bool
syncdir_handle_(
  upd_req_t* req);

static
size_t
syncdir_stack_child_path_(
  upd_file_t*    f,
  uint8_t**      dst,
  const uint8_t* name,
  size_t         len);

static
size_t
syncdir_stack_child_npath_(
  upd_file_t*    f,
  uint8_t**      dst,
  const uint8_t* name,
  size_t         len);

static
bool
syncdir_find_(
  upd_file_t*          f,
  upd_req_dir_entry_t* e);

static
const rule_t_*
syncdir_select_rule_(
  upd_file_t*        f,
  const uv_dirent_t* e);

static
upd_file_t*
syncdir_create_file_from_rule_(
  upd_file_t*    f,
  const rule_t_* rule,
  upd_file_t*    proto);

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
syncdir_logf_(
  upd_file_t* f,
  const char* fmt,
  ...);

static
bool
syncdir_sync_n2u_(
  upd_file_t* f,
  upd_req_t*  req);

static
void
syncdir_finalize_sync_(
  upd_file_t* f);

const upd_driver_t upd_driver_syncdir = {
  .name = (uint8_t*) "upd.syncdir",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_DIR,
    0,
  },
  .flags = {
    .npoll = true,
  },
  .init   = syncdir_init_,
  .deinit = syncdir_deinit_,
  .handle = syncdir_handle_,
};


static
void
syncdir_watch_cb_(
  upd_file_watch_t* w);

static
void
syncdir_open_cb_(
  uv_fs_t* fsreq);

static
void
syncdir_close_cb_(
  uv_fs_t* fsreq);

static
void
syncdir_mkdir_cb_(
  uv_fs_t* fsreq);

static
void
syncdir_sync_n2u_lock_cb_(
  upd_file_lock_t* lock);

static
void
syncdir_sync_n2u_scandir_cb_(
  uv_fs_t* fsreq);


static bool syncdir_parse_param_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  ctx_t_*    ctx = f->ctx;

  if (HEDLEY_UNLIKELY(f->paramlen == 0)) {
    return false;
  }

  yaml_document_t doc = {0};
  if (HEDLEY_UNLIKELY(!upd_yaml_parse(&doc, f->param, f->paramlen))) {
    syncdir_logf_(f, "param parse failure");
    return false;
  }

  const yaml_node_t* node = yaml_document_get_root_node(&doc);
  if (HEDLEY_UNLIKELY(!node || node->type != YAML_MAPPING_NODE)) {
    yaml_document_delete(&doc);
    syncdir_logf_(f, "expected syncdir rule list (mapping)");
    return false;
  }

  const yaml_node_pair_t* itr = node->data.mapping.pairs.start;
  const yaml_node_pair_t* end = node->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    const yaml_node_t* k = yaml_document_get_node(&doc, itr->key);
    const yaml_node_t* v = yaml_document_get_node(&doc, itr->value);

    if (HEDLEY_UNLIKELY(!k || k->type != YAML_SCALAR_NODE)) {
      syncdir_logf_(f, "found non-string key, skipping");
      continue;
    }

    const uint8_t* pattern    = k->data.scalar.value;
    const size_t   patternlen = k->data.scalar.length;

    if (HEDLEY_UNLIKELY(!v || v->type != YAML_SEQUENCE_NODE)) {
      syncdir_logf_(f,
        "found non-sequence value, skipping '%.*s'", (int) patternlen, pattern);
      continue;
    }

    rule_t_* rule = NULL;
    if (HEDLEY_UNLIKELY(!upd_malloc(&rule, sizeof(*rule)+patternlen+1))) {
      syncdir_logf_(f, "rule allocation failure, skipping");
      continue;
    }
    *rule = (rule_t_) {
      .pattern = utf8ncpy(rule+1, pattern, patternlen),
    };
    rule->pattern[patternlen] = 0;

    const yaml_node_item_t* itr = v->data.sequence.items.start;
    const yaml_node_item_t* end = v->data.sequence.items.top;
    for (; itr < end; ++itr) {
      const yaml_node_t* v = yaml_document_get_node(&doc, *itr);
      if (HEDLEY_UNLIKELY(!v)) continue;

      const yaml_node_t* driver = NULL;
      const yaml_node_t* param  = NULL;

      switch (v->type) {
      case YAML_SCALAR_NODE:
        driver = v;
        break;

      case YAML_MAPPING_NODE: {
        const char* invalid =
          upd_yaml_find_fields(&doc, v, (upd_yaml_field_t[]) {
              { .name = "driver", .required = true,  .str = &driver, },
              { .name = "param",  .required = false, .str = &param,  },
              { NULL, },
            });
        if (HEDLEY_UNLIKELY(invalid)) {
          syncdir_logf_(f,
            "invalid field '%s' in pattern '%.*s'",
            invalid, (int) patternlen, pattern);
          goto ABORT_PATTERN;
        }
      } break;

      default:
        syncdir_logf_(f,
          "expected scalar or mapping in pattern '%.*s'",
          (int) patternlen, pattern);
        goto ABORT_PATTERN;
      }

      const uint8_t* dname    = driver->data.scalar.value;
      const size_t   dnamelen = driver->data.scalar.length;
      const upd_driver_t* d = upd_driver_lookup(iso, dname, dnamelen);
      if (HEDLEY_UNLIKELY(d == NULL)) {
        syncdir_logf_(f, "unknown driver '%.*s' in pattern '%.*s'",
          (int) dnamelen, dname, (int) patternlen, pattern);
        break;
      }

      const uint8_t* p    = NULL;
      size_t         plen = 0;
      if (param) {
        p    = param->data.scalar.value;
        plen = param->data.scalar.length;
      }

      rule_item_t_* item = NULL;
      if (HEDLEY_UNLIKELY(!upd_malloc(&item, sizeof(*item)+plen+1))) {
        syncdir_logf_(f, "rule item allocation failure");
        break;
      }
      *item = (rule_item_t_) {
        .driver = d,
        .param  = utf8ncpy(item+1, p, plen),
      };
      item->param[plen] = 0;

      if (HEDLEY_UNLIKELY(!upd_array_insert(&rule->items, item, SIZE_MAX))) {
        upd_free(&item);
        syncdir_logf_(f, "rule item insertion failure");
        break;
      }
    }

ABORT_PATTERN:
    if (HEDLEY_UNLIKELY(rule->items.n == 0)) {
      upd_free(&rule);
      syncdir_logf_(f,
        "pattern '%.*s' has no configuration", (int) patternlen, pattern);
      continue;
    }
    if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->rules, rule, SIZE_MAX))) {
      upd_free(&rule);
      syncdir_logf_(f, "rule insertion failure, skipping");
      continue;
    }
  }

  yaml_document_delete(&doc);

  if (HEDLEY_UNLIKELY(ctx->rules.n == 0)) {
    syncdir_logf_(f, "syncdir has no rules");
    return false;
  }
  return true;
}

static bool syncdir_init_(upd_file_t* f) {
  if (HEDLEY_UNLIKELY(f->npath == NULL || f->path == NULL)) {
    return false;
  }

  ctx_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (ctx_t_) {
    .watch = {
      .file  = f,
      .udata = f,
      .cb    = syncdir_watch_cb_,
    },
  };
  f->ctx = ctx;

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    return false;
  }
  if (HEDLEY_UNLIKELY(!syncdir_parse_param_(f))) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    return false;
  }

  if (HEDLEY_UNLIKELY(!syncdir_sync_n2u_(f, NULL))) {
    syncdir_logf_(f, "first sync failed");
  }
  return true;
}

static void syncdir_deinit_(upd_file_t* f) {
  ctx_t_* ctx = f->ctx;

  for (size_t i = 0; i < ctx->children.n; ++i) {
    upd_req_dir_entry_t* e = ctx->children.p[i];
    if (HEDLEY_UNLIKELY(e->file->driver == &upd_driver_syncdir)) {
      ctx_t_* subctx = e->file->ctx;
      subctx->parent = NULL;
    }
    upd_file_unref(e->file);
    upd_free(&e);
  }
  upd_array_clear(&ctx->children);

  for (size_t i = 0; i < ctx->rules.n; ++i) {
    rule_t_* r = ctx->rules.p[i];
    for (size_t j = 0; j < r->items.n; ++j) {
      rule_item_t_* item = r->items.p[j];
      upd_free(&item);
    }
    upd_array_clear(&r->items);
    upd_free(&r);
  }
  upd_array_clear(&ctx->rules);

  upd_free(&ctx);
}

static bool syncdir_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  ctx_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  switch (req->type) {
  case UPD_REQ_DIR_LIST:
    req->dir.entries = (upd_req_dir_entries_t) {
      .p = (upd_req_dir_entry_t**) ctx->children.p,
      .n = ctx->children.n,
    };
    req->result = UPD_REQ_OK;
    req->cb(req);
    return true;

  case UPD_REQ_DIR_FIND:
    syncdir_find_(f, &req->dir.entry);
    req->result = UPD_REQ_OK;
    req->cb(req);
    return true;

  case UPD_REQ_DIR_NEW:
  case UPD_REQ_DIR_NEWDIR: {
    const upd_req_dir_entry_t* e = &req->dir.entry;

    uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
    if (HEDLEY_UNLIKELY(fsreq == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    *fsreq = (uv_fs_t) { .data = req, };

    uint8_t* npath = NULL;
    syncdir_stack_child_npath_(f, &npath, e->name, e->len);

    bool open;
    if (req->type == UPD_REQ_DIR_NEWDIR) {
      open = 0 <= uv_fs_mkdir(
        &iso->loop, fsreq, (char*) npath, S_IFDIR, syncdir_mkdir_cb_);
    } else {
      open = 0 <= uv_fs_open(
        &iso->loop,
        fsreq,
        (char*) npath,
        O_CREAT | O_EXCL | O_WRONLY,
        DEFAULT_PERMISSION_,
        syncdir_open_cb_);
    }
    upd_iso_unstack(iso, npath);
    if (HEDLEY_UNLIKELY(!open)) {
      upd_iso_unstack(iso, fsreq);
      req->result = UPD_REQ_ABORTED;
      return false;
    }
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static size_t syncdir_stack_child_path_(
    upd_file_t* f, uint8_t** dst, const uint8_t* name, size_t len) {
  upd_iso_t* iso = f->iso;

  const uint8_t* par    = f->path;
  const size_t   parlen = f->pathlen;

  const size_t ret = parlen+1 + len+1;

  *dst = upd_iso_stack(iso, ret);
  if (HEDLEY_UNLIKELY(*dst == NULL)) {
    return 0;
  }
  utf8ncpy(*dst, par, parlen);
  (*dst)[parlen] = '/';
  utf8ncpy(*dst+parlen+1, name, len);
  (*dst)[ret-1] = 0;
  return ret;
}

static size_t syncdir_stack_child_npath_(
    upd_file_t* f, uint8_t** dst, const uint8_t* name, size_t len) {
  upd_iso_t* iso = f->iso;

  uint8_t* term = upd_iso_stack(iso, len+1);
  if (HEDLEY_UNLIKELY(term == NULL)) {
    return 0;
  }
  utf8ncpy(term, name, len);
  term[len] = 0;

  const size_t dstlen = cwk_path_join((char*) f->npath, (char*) term, NULL, 0);
  *dst = upd_iso_stack(iso, dstlen+1);
  if (HEDLEY_UNLIKELY(*dst == NULL)) {
    upd_iso_unstack(iso, term);
    return 0;
  }
  cwk_path_join((char*) f->npath, (char*) term, (char*) *dst, dstlen+1);
  upd_iso_unstack(iso, term);

  return dstlen;
}

static bool syncdir_find_(upd_file_t* f, upd_req_dir_entry_t* e) {
  ctx_t_* ctx = f->ctx;

  for (size_t i = 0; i < ctx->children.n; ++i) {
    const upd_req_dir_entry_t* g = ctx->children.p[i];
    if (HEDLEY_UNLIKELY(upd_streq(e->name, e->len, g->name, g->len))) {
      *e = *g;
      return true;
    }
  }
  *e = (upd_req_dir_entry_t) {0};
  return false;
}

static const rule_t_* syncdir_select_rule_(
    upd_file_t* f, const uv_dirent_t* e) {
  ctx_t_* ctx = f->ctx;

  for (size_t i = 0; i < ctx->rules.n; ++i) {
    const rule_t_* r = ctx->rules.p[i];

    if (HEDLEY_LIKELY(r->pattern[0])) {
      int matchlen = 0;
      re_match((char*) r->pattern, e->name, &matchlen);
      if (HEDLEY_UNLIKELY(e->name[matchlen] == 0)) {
        return r;
      }
    } else {
      return r;
    }
  }
  if (HEDLEY_UNLIKELY(ctx->parent)) {
    return syncdir_select_rule_(ctx->parent, e);
  }
  return NULL;
}

static upd_file_t* syncdir_create_file_from_rule_(
    upd_file_t* f, const rule_t_* rule, upd_file_t* proto) {
  upd_file_t* fc = NULL;
  for (ssize_t i = rule->items.n-1; i >= 0; --i) {
    rule_item_t_* item = rule->items.p[i];

    proto->driver   = item->driver;
    proto->param    = item->param;
    proto->paramlen = item->paramlen;
    proto->backend  = fc;

    upd_file_t* temp = upd_file_new(proto);
    if (fc) upd_file_unref(fc);

    if (HEDLEY_UNLIKELY(temp == NULL)) {
      return NULL;
    }
    if (HEDLEY_UNLIKELY(temp->driver == &upd_driver_syncdir)) {
      ctx_t_* subctx = fc->ctx;
      subctx->parent = f;
    }
    fc = temp;
  }
  return fc;
}

static void syncdir_logf_(upd_file_t* f, const char* fmt, ...) {
  uint8_t msg[256];

  va_list args;
  va_start(args, fmt);
  vsnprintf((char*) msg, sizeof(msg), fmt, args);
  va_end(args);

  upd_iso_msgf(f->iso, "upd.syncdir: %s (%s)\n", msg, f->npath);
}

static bool syncdir_sync_n2u_(upd_file_t* f, upd_req_t* req) {
  ctx_t_* ctx = f->ctx;

  if (req) {
    if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->reqs, req, SIZE_MAX))) {
      syncdir_logf_(f, "req insertion failure");
      return false;
    }
  }
  if (HEDLEY_UNLIKELY(ctx->busy)) {
    return true;
  }

  ctx->lock = (upd_file_lock_t) {
    .file  = f,
    .udata = f,
    .cb    = syncdir_sync_n2u_lock_cb_,
  };
  ctx->selflock = !req;
  ctx->busy     = true;

  if (ctx->selflock) {
    if (HEDLEY_UNLIKELY(!upd_file_lock(&ctx->lock))) {
      syncdir_logf_(f, "self lock failure");
      ctx->busy = false;
      return false;
    }
  } else {
    syncdir_sync_n2u_lock_cb_(&ctx->lock);
  }
  return true;
}

static void syncdir_finalize_sync_(upd_file_t* f) {
  ctx_t_* ctx = f->ctx;

  ctx->busy = false;

  if (ctx->selflock) {
    upd_file_unlock(&ctx->lock);
  }

  for (size_t i = 0; i < ctx->reqs.n; ++i) {
    upd_req_t* req = ctx->reqs.p[i];
    syncdir_find_(f, &req->dir.entry);

    if (HEDLEY_UNLIKELY(req->type == UPD_REQ_DIR_FIND)) {
      req->result = UPD_REQ_OK;
    } else {
      req->result = req->dir.entry.file? UPD_REQ_OK: UPD_REQ_ABORTED;
    }
    req->cb(req);
  }
  upd_array_clear(&ctx->reqs);
}


static void syncdir_watch_cb_(upd_file_watch_t* w) {
  upd_file_t* f = w->udata;

  if (HEDLEY_UNLIKELY(w->event == UPD_FILE_UPDATE_N)) {
    if (HEDLEY_UNLIKELY(!syncdir_sync_n2u_(f, NULL))) {
      syncdir_logf_(f, "auto sync failure");
      return;
    }
  }
}

static void syncdir_open_cb_(uv_fs_t* fsreq) {
  upd_req_t*  req = fsreq->data;
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    upd_iso_unstack(iso, fsreq);
    goto ABORT;
  }

  fsreq->data = f;
  const int close = uv_fs_close(
    &iso->loop, fsreq, result, syncdir_close_cb_);
  if (HEDLEY_UNLIKELY(close < 0)) {
    upd_iso_unstack(iso, fsreq);
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(!syncdir_sync_n2u_(f, req))) {
    goto ABORT;
  }
  return;

ABORT:
  req->result = UPD_REQ_ABORTED;
  req->cb(req);
}

static void syncdir_close_cb_(uv_fs_t* fsreq) {
  upd_file_t* f = fsreq->data;

  uv_fs_req_cleanup(fsreq);
  upd_iso_unstack(f->iso, fsreq);
}

static void syncdir_mkdir_cb_(uv_fs_t* fsreq) {
  upd_req_t*  req = fsreq->data;
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!syncdir_sync_n2u_(f, req))) {
    goto ABORT;
  }
  goto EXIT;

ABORT:
  req->result = UPD_REQ_ABORTED;
  req->cb(req);

EXIT:
  upd_iso_unstack(iso, fsreq);
}

static void syncdir_sync_n2u_lock_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->selflock != k->ok)) {
    goto ABORT;
  }

  uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
  if (HEDLEY_UNLIKELY(fsreq == NULL)) {
    goto ABORT;
  }
  *fsreq = (uv_fs_t) { .data = f, };

  const int scandir = uv_fs_scandir(&iso->loop,
    fsreq, (char*) f->npath, 0, syncdir_sync_n2u_scandir_cb_);
  if (HEDLEY_UNLIKELY(scandir < 0)) {
    upd_iso_unstack(iso, fsreq);
    goto ABORT;
  }
  return;

ABORT:
  syncdir_finalize_sync_(f);
}

static void syncdir_sync_n2u_scandir_cb_(uv_fs_t* fsreq) {
  upd_file_t* f   = fsreq->data;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  bool* rm       = NULL;
  bool  modified = false;

  if (HEDLEY_UNLIKELY(fsreq->result < 0)) {
    goto EXIT;
  }

  const size_t prev_un = ctx->children.n;

  rm = prev_un? upd_iso_stack(iso, sizeof(*rm)*prev_un): NULL;
  if (HEDLEY_UNLIKELY(prev_un && rm == NULL)) {
    goto EXIT;
  }
  for (size_t i = 0; i < prev_un; ++i) {
    rm[i] = true;
  }

  for (size_t n = 0; n < (size_t) fsreq->result; ++n) {
    uv_dirent_t ne;
    if (HEDLEY_UNLIKELY(0 > uv_fs_scandir_next(fsreq, &ne))) {
      break;
    }

    const bool dir = ne.type == UV_DIRENT_DIR;
    if (HEDLEY_UNLIKELY(!dir && ne.type != UV_DIRENT_FILE)) {
      syncdir_logf_(f, "'%s' is ignored because of unknown file type", ne.name);
      continue;
    }

    bool found = false;
    for (size_t u = 0; u < ctx->children.n; ++u) {
      upd_req_dir_entry_t* ue = ctx->children.p[u];
      if (HEDLEY_UNLIKELY(utf8cmp(ne.name, ue->name) == 0)) {
        found = true;
        if (HEDLEY_LIKELY(u < prev_un)) {
          rm[u] = false;
        }
        break;
      }
    }

    if (HEDLEY_UNLIKELY(!found)) {
      const rule_t_* rule = NULL;
      rule = syncdir_select_rule_(f, &ne);
      if (HEDLEY_UNLIKELY(rule == NULL)) {
        syncdir_logf_(f, "no pattern matched, '%s' is ignored", ne.name);
        continue;
      }

      uint8_t* path;
      const size_t pathlen = syncdir_stack_child_path_(
        f, &path, (uint8_t*) ne.name, utf8size_lazy(ne.name));
      if (HEDLEY_UNLIKELY(pathlen == 0)) {
        syncdir_logf_(f, "path allocation failure, '%s' is ignored", ne.name);
        continue;
      }

      uint8_t* npath;
      const size_t npathlen = syncdir_stack_child_npath_(
        f, &npath, (uint8_t*) ne.name, utf8size_lazy(ne.name));
      if (HEDLEY_UNLIKELY(npathlen == 0)) {
        upd_iso_unstack(iso, path);
        syncdir_logf_(f, "npath allocation failure, '%s' is ignored", ne.name);
        continue;
      }

      upd_file_t* fc = syncdir_create_file_from_rule_(f, rule, &(upd_file_t) {
          .iso      = iso,
          .path     = path,
          .pathlen  = pathlen,
          .npath    = npath,
          .npathlen = npathlen,
        });
      upd_iso_unstack(iso, path);
      upd_iso_unstack(iso, npath);

      if (HEDLEY_UNLIKELY(fc == NULL)) {
        syncdir_logf_(f, "file creation failed, '%s' is ignored", ne.name);
        continue;
      }

      const size_t         len = utf8size_lazy(ne.name);
      upd_req_dir_entry_t* e   = NULL;
      if (HEDLEY_UNLIKELY(!upd_malloc(&e, sizeof(*e)+len+1))) {
        upd_file_unref(fc);
        syncdir_logf_(f, "entry allocation failed, '%s' is ignored", ne.name);
        continue;
      }
      *e = (upd_req_dir_entry_t) {
        .name = (uint8_t*) (e+1),
        .len  = len,
        .file = fc,
      };
      utf8ncpy(e->name, ne.name, len);
      e->name[len] = 0;

      if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->children, e, SIZE_MAX))) {
        upd_file_unref(fc);
        upd_free(&e);
        syncdir_logf_(f, "entry insertion failed, '%s' is ignored", ne.name);
        continue;
      }
      modified = true;
    }
  }

  for (size_t i = prev_un; i > 0;) {
    --i;
    if (HEDLEY_UNLIKELY(rm[i])) {
      upd_req_dir_entry_t* e = upd_array_remove(&ctx->children, i);
      if (HEDLEY_UNLIKELY(e == NULL)) {
        continue;
      }
      upd_file_unref(e->file);
      upd_free(&e);
      modified = true;
    }
  }

EXIT:
  if (HEDLEY_LIKELY(rm)) {
    upd_iso_unstack(iso, rm);
  }

  uv_fs_req_cleanup(fsreq);
  upd_iso_unstack(iso, fsreq);

  if (modified) {
    upd_file_trigger(f, UPD_FILE_UPDATE);
  }

  syncdir_finalize_sync_(f);
}
