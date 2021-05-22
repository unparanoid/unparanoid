#include "common.h"


#define CONFIG_FILE_     "upd.yml"

#define CONFIG_FILE_MAX_ (1024*1024*4)  /* = 4 MiB */


typedef struct ctx_t_  ctx_t_;
typedef struct task_t_ task_t_;

struct ctx_t_ {
  upd_iso_t* iso;

  const uint8_t* path;
  size_t         pathlen;
  const uint8_t* fpath;
  size_t         fpathlen;

  uv_file  fd;
  uint8_t* buf;
  size_t   size;

  uv_fs_t fsreq;

  yaml_document_t doc;
  size_t          refcnt;
};

struct task_t_ {
  ctx_t_*      ctx;
  yaml_node_t* node;

  union {
    struct {
      const uint8_t* upath;
      size_t         ulen;
      uint16_t       port;
    } server;

    struct {
      const uint8_t* upath;
      size_t         ulen;
      size_t         uoffset;

      const uint8_t* npath;
      size_t         nlen;

      yaml_node_t* rules;
    } sync;
  };
};


static
void
config_unref_(
  ctx_t_* ctx);

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
config_logf_(
  ctx_t_*     ctx,
  const char* fmt,
  ...);

HEDLEY_PRINTF_FORMAT(3, 4)
static
void
config_lognf_(
  ctx_t_*      ctx,
  yaml_node_t* n,
  const char*  fmt,
  ...);

static
void
config_parse_(
  ctx_t_* ctx);

static
void
config_parse_server_(
  ctx_t_*      ctx,
  yaml_node_t* node);

static
void
config_parse_sync_(
  ctx_t_*      ctx,
  yaml_node_t* node);


static
void
config_parse_server_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
config_parse_sync_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
config_parse_sync_lock_for_add_cb_(
  upd_file_lock_t* lock);

static
void
config_parse_sync_add_cb_(
  upd_req_t* req);


static
void
config_stat_cb_(
  uv_fs_t* req);

static
void
config_open_cb_(
  uv_fs_t* req);

static
void
config_read_cb_(
  uv_fs_t* req);

static
void
config_close_cb_(
  uv_fs_t* req);


bool upd_config_load_from_path(upd_iso_t* iso, const uint8_t* path) {
  /* Adds a surplus of 8 bytes for path join. */
  const size_t fpathlen = cwk_path_join((char*) path, CONFIG_FILE_, NULL, 0);

  ctx_t_* ctx = upd_iso_stack(iso, sizeof(*ctx) + fpathlen + 1);
  if (HEDLEY_UNLIKELY(ctx == NULL)) {
    return false;
  }
  *ctx = (ctx_t_) {
    .iso      = iso,
    .path     = (uint8_t*) (ctx+1),
    .fpath    = (uint8_t*) (ctx+1),
    .fpathlen = fpathlen,
    .fsreq    = { .data = ctx, },
    .refcnt   = 1,
  };
  cwk_path_join((char*) path, CONFIG_FILE_, (char*) ctx->fpath, fpathlen+1);
  cwk_path_get_dirname((char*) ctx->fpath, &ctx->pathlen);

  const bool stat = 0 <= uv_fs_stat(
    &iso->loop, &ctx->fsreq, (char*) ctx->fpath, config_stat_cb_);
  if (HEDLEY_UNLIKELY(!stat)) {
    upd_iso_unstack(iso, ctx);
    return false;
  }
  return true;
}


static void config_unref_(ctx_t_* ctx) {
  if (HEDLEY_UNLIKELY(--ctx->refcnt == 0)) {
    yaml_document_delete(&ctx->doc);
    upd_iso_unstack(ctx->iso, ctx);
  }
}

static void config_logf_(ctx_t_* ctx, const char* fmt, ...) {
  upd_iso_msgf(ctx->iso, "config error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(ctx->iso, fmt, args);
  va_end(args);

  upd_iso_msgf(ctx->iso, " (%s)\n", ctx->path);
}

static void config_lognf_(ctx_t_* ctx, yaml_node_t* n, const char* fmt, ...) {
  upd_iso_msgf(ctx->iso, "config error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(ctx->iso, fmt, args);
  va_end(args);

  upd_iso_msgf(ctx->iso,
    " (%s/"CONFIG_FILE_":%zu:%zu)\n",
    ctx->path, n->start_mark.line+1, n->start_mark.column+1);
}

static void config_parse_(ctx_t_* ctx) {
  static const struct {
    const char* name;
    void
    (*func)(
      ctx_t_*      ctx,
      yaml_node_t* node);
  } subparsers[] = {
    { "server", config_parse_server_, },
    { "sync",   config_parse_sync_,   },
  };

  yaml_document_t* doc = &ctx->doc;

  yaml_node_t* root = yaml_document_get_root_node(&ctx->doc);
  if (HEDLEY_UNLIKELY(root->type != YAML_MAPPING_NODE)) {
    config_logf_(ctx, "yaml root is not mapping");
    return;
  }
  yaml_node_pair_t* itr = root->data.mapping.pairs.start;
  yaml_node_pair_t* end = root->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(doc, itr->value);
    if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
      config_logf_(ctx, "key must be scalar");
      continue;
    }
    const uint8_t* name    = key->data.scalar.value;
    const size_t   namelen = key->data.scalar.length;

    bool handled = false;
    for (size_t i = 0; i < sizeof(subparsers)/sizeof(subparsers[0]); ++i) {
      const bool match =
        utf8ncmp(subparsers[i].name, name, namelen) == 0 &&
        utf8size_lazy(subparsers[i].name) == namelen;
      if (HEDLEY_UNLIKELY(match)) {
        subparsers[i].func(ctx, val);
        handled = true;
        break;
      }
    }
    if (HEDLEY_UNLIKELY(!handled)) {
      config_lognf_(ctx, key, "unknown block '%.*s'", (int) namelen, name);
      continue;
    }
  }
}

static void config_parse_server_(ctx_t_* ctx, yaml_node_t* node) {
  upd_iso_t*       iso = ctx->iso;
  yaml_document_t* doc = &ctx->doc;

  if (HEDLEY_UNLIKELY(!node)) {
    return;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_MAPPING_NODE)) {
    config_lognf_(ctx, node, "invalid server specification");
    return;
  }

  yaml_node_pair_t* itr = node->data.mapping.pairs.start;
  yaml_node_pair_t* end = node->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(doc, itr->value);
    if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, node, "key must be scalar");
      continue;
    }

    const uint8_t* name    = key->data.scalar.value;
    const size_t   namelen = key->data.scalar.length;

    if (HEDLEY_UNLIKELY(!val || val->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, key,
        "scalar expected for value of '%.*s'", (int) namelen, name);
      continue;
    }

    const uint8_t* value    = val->data.scalar.value;
    const size_t   valuelen = val->data.scalar.length;

    char* temp;
    const uintmax_t port = strtoumax((char*) value, &temp, 0);
    if (HEDLEY_UNLIKELY(*temp != 0 || port == 0 || port > UINT16_MAX)) {
      config_lognf_(ctx, val,
        "invalid port specification '%.*s'", (int) valuelen, value);
      continue;
    }

    task_t_* task = upd_iso_stack(iso, sizeof(*task));
    if (HEDLEY_UNLIKELY(task == NULL)) {
      config_lognf_(ctx, key, "task allocation failure");
      break;
    }
    *task = (task_t_) {
      .ctx  = ctx,
      .node = key,
      .server = {
        .upath = name,
        .ulen  = namelen,
        .port  = port,
      },
    };

    ++ctx->refcnt;
    const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
        .iso   = ctx->iso,
        .path  = (uint8_t*) name,
        .len   = namelen,
        .udata = task,
        .cb    = config_parse_server_pathfind_cb_,
      });
    if (HEDLEY_UNLIKELY(!pf)) {
      upd_iso_unstack(iso, task);
      config_unref_(ctx);
      config_lognf_(ctx, key, "pathfind failure");
      continue;
    }
  }
}

static void config_parse_sync_(ctx_t_* ctx, yaml_node_t* node) {
  upd_iso_t*       iso = ctx->iso;
  yaml_document_t* doc = &ctx->doc;

  if (HEDLEY_UNLIKELY(!node)) {
    return;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_MAPPING_NODE)) {
    config_lognf_(ctx, node, "invalid sync specification");
    return;
  }

  yaml_node_pair_t* itr = node->data.mapping.pairs.start;
  yaml_node_pair_t* end = node->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(doc, itr->value);
    if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, node, "key must be scalar");
      continue;
    }

    const uint8_t* name    = key->data.scalar.value;
    const size_t   namelen = key->data.scalar.length;
    const size_t   dirlen  = upd_path_dirname((uint8_t*) name, namelen);

    if (HEDLEY_UNLIKELY(!val || val->type != YAML_MAPPING_NODE)) {
      config_lognf_(ctx, val,
        "mapping expected for value of '%.*s'", (int) namelen, name);
      continue;
    }

    task_t_* task = upd_iso_stack(iso, sizeof(*task));
    if (HEDLEY_UNLIKELY(task == NULL)) {
      config_lognf_(ctx, key, "task allocation failure");
      break;
    }
    *task = (task_t_) {
      .ctx  = ctx,
      .node = key,
      .sync = {
        .upath   = name,
        .ulen    = namelen,
        .uoffset = dirlen,
      },
    };

    yaml_node_pair_t* itr = val->data.mapping.pairs.start;
    yaml_node_pair_t* end = val->data.mapping.pairs.top;
    for (; itr < end; ++itr) {
      yaml_node_t* key = yaml_document_get_node(doc, itr->key);
      yaml_node_t* val = yaml_document_get_node(doc, itr->value);
      if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
        config_lognf_(ctx, node, "key must be scalar");
        continue;
      }

      const uint8_t* name    = key->data.scalar.value;
      const size_t   namelen = key->data.scalar.length;

#     define nameq_(v) (utf8ncmp(name, v, namelen) == 0 && v[namelen] == 0)

      if (nameq_("npath")) {
        if (HEDLEY_UNLIKELY(!val || val->type != YAML_SCALAR_NODE)) {
          config_lognf_(ctx, key, "scalar expected for 'npath' field");
          continue;
        }
        task->sync.npath = val->data.scalar.value;
        task->sync.nlen  = val->data.scalar.length;

      } else if (nameq_("rules")) {
        if (HEDLEY_UNLIKELY(!val || val->type != YAML_MAPPING_NODE)) {
          config_lognf_(ctx, key, "mapping expected for 'rules' field");
          continue;
        }
        task->sync.rules = val;

      } else {
        config_lognf_(ctx, key, "unknown field '%.*s'", (int) namelen, name);
        continue;
      }

#     undef nameq_
    }

    if (HEDLEY_UNLIKELY(task->sync.nlen == 0 || task->sync.rules == NULL)) {
      upd_iso_unstack(iso, task);
      config_lognf_(ctx, node, "requires 'npath' and 'rules' field");
      continue;
    }

    ++ctx->refcnt;
    const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
        .iso    = iso,
        .path   = (uint8_t*) name,
        .len    = dirlen,
        .create = true,
        .udata  = task,
        .cb     = config_parse_sync_pathfind_cb_,
      });
    if (HEDLEY_UNLIKELY(!pf)) {
      upd_iso_unstack(iso, task);
      config_unref_(ctx);
      config_lognf_(ctx, node, "patfind failure");
      continue;
    }
  }
}


static void config_parse_server_pathfind_cb_(upd_req_pathfind_t* pf) {
  task_t_*   task = pf->udata;
  ctx_t_*    ctx  = task->ctx;
  upd_iso_t* iso  = ctx->iso;

  yaml_node_t*   node  = task->node;
  const uint16_t port  = task->server.port;
  const uint8_t* upath = task->server.upath;
  const size_t   ulen  = task->server.ulen;

  upd_file_t* file = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);
  upd_iso_unstack(iso, task);

  if (HEDLEY_UNLIKELY(!file)) {
    config_lognf_(ctx, node, "unknown program '%.*s'", (int) ulen, upath);
    goto EXIT;
  }

  const bool srv = upd_srv_new_tcp(iso, file, (uint8_t*) "0.0.0.0", port);
  if (HEDLEY_UNLIKELY(!srv)) {
    config_lognf_(ctx, node, "failed to start server on tcp %"PRIu16, port);
    goto EXIT;
  }

EXIT:
  config_unref_(ctx);
}

static void config_parse_sync_pathfind_cb_(upd_req_pathfind_t* pf) {
  task_t_*   task = pf->udata;
  ctx_t_*    ctx  = task->ctx;
  upd_iso_t* iso  = ctx->iso;

  yaml_node_t* node = task->node;

  upd_file_t* base = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(!base)) {
    config_lognf_(ctx, node, "parent dir creation failure");
    goto ABORT;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = base,
      .ex    = true,
      .udata = task,
      .cb    = config_parse_sync_lock_for_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    config_lognf_(ctx, node, "parent dir lock failure");
    goto ABORT;
  }
  return;

ABORT:
  upd_iso_unstack(iso, task);
  config_unref_(ctx);
}

static void config_parse_sync_lock_for_add_cb_(upd_file_lock_t* lock) {
  task_t_*   task = lock->udata;
  ctx_t_*    ctx  = task->ctx;
  upd_iso_t* iso  = ctx->iso;

  yaml_document_t* doc  = &ctx->doc;
  yaml_node_t*     node = task->node;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    config_lognf_(ctx, node, "lock failure");
    goto ABORT;
  }

  upd_file_t* f = upd_file_new_from_npath(
    iso, &upd_driver_syncdir, task->sync.npath, task->sync.nlen);
  if (HEDLEY_UNLIKELY(!f)) {
    config_lognf_(ctx, node, "syncdir file creation failure");
    goto ABORT;
  }

  upd_array_of(upd_driver_rule_t*) rules = {0};

  yaml_node_pair_t* itr = task->sync.rules->data.mapping.pairs.start;
  yaml_node_pair_t* end = task->sync.rules->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(doc, itr->value);

    if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, node, "key must be scalar");
      continue;
    }
    if (HEDLEY_UNLIKELY(!val || val->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, key, "scalar expected for driver name");
      continue;
    }

    const uint8_t* value    = val->data.scalar.value;
    const size_t   valuelen = val->data.scalar.length;

    const upd_driver_t* driver = upd_driver_lookup(iso, value, valuelen);
    if (HEDLEY_UNLIKELY(driver == NULL)) {
      config_lognf_(ctx, val,
        "unknown driver name '%.*s'", (int) valuelen, value);
      continue;
    }

    const size_t tail = key->data.scalar.length + 1;

    upd_driver_rule_t* r = NULL;
    if (HEDLEY_UNLIKELY(!upd_malloc(&r, sizeof(*r)+tail))) {
      config_lognf_(ctx, val, "driver rule allocation failure");
      break;
    }
    *r = (upd_driver_rule_t) {
      .ext    = (uint8_t*) (r+1),
      .len    = key->data.scalar.length,
      .driver = driver,
    };
    utf8ncpy(r->ext, key->data.scalar.value, r->len);
    r->ext[r->len] = 0;

    if (HEDLEY_UNLIKELY(!upd_array_insert(&rules, r, SIZE_MAX))) {
      upd_free(&r);
      config_lognf_(ctx, val, "driver rule insertion failure");
      break;
    }
  }
  upd_driver_syncdir_set_rules(f, &rules);  /* ownership moves */

  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file = lock->file,
      .type = UPD_REQ_DIR_ADD,
      .dir  = { .entry = {
        .file = f,
        .name = (uint8_t*) (task->sync.upath + task->sync.uoffset),
        .len  = task->sync.ulen - task->sync.uoffset,
      }, },
      .udata = lock,
      .cb    = config_parse_sync_add_cb_,
    });
  upd_file_unref(f);

  if (HEDLEY_UNLIKELY(!add)) {
    config_lognf_(ctx, node, "add request refused");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  upd_iso_unstack(iso, task);

  config_unref_(ctx);
}

static void config_parse_sync_add_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  task_t_*         task = lock->udata;
  ctx_t_*          ctx  = task->ctx;
  upd_iso_t*       iso  = ctx->iso;

  yaml_node_t* node = task->node;

  const bool added = req->result == UPD_REQ_OK;
  upd_iso_unstack(iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  if (HEDLEY_UNLIKELY(!added)) {
    config_lognf_(ctx, node, "add request failure");
    goto EXIT;
  }

EXIT:
  upd_iso_unstack(iso, task);
  config_unref_(ctx);
}


static void config_stat_cb_(uv_fs_t* req) {
  ctx_t_*    ctx = req->data;
  upd_iso_t* iso = ctx->iso;

  const ssize_t result = req->result;

  ctx->size = req->statbuf.st_size;
  uv_fs_req_cleanup(req);

  if (HEDLEY_UNLIKELY(result != 0)) {
    config_logf_(ctx, "stat failure");
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(ctx->size > CONFIG_FILE_MAX_)) {
    config_logf_(ctx,
      "too large config (must be %d bytes or less)", CONFIG_FILE_MAX_);
    goto ABORT;
  }

  const bool open = 0 <= uv_fs_open(
    &iso->loop, &ctx->fsreq, (char*) ctx->fpath, O_RDONLY, 0, config_open_cb_);
  if (HEDLEY_UNLIKELY(!open)) {
    config_logf_(ctx, "open failure");
    goto ABORT;
  }
  return;

ABORT:
  config_unref_(ctx);
}

static void config_open_cb_(uv_fs_t* req) {
  ctx_t_*    ctx = req->data;
  upd_iso_t* iso = ctx->iso;

  const ssize_t result = req->result;
  uv_fs_req_cleanup(req);

  if (HEDLEY_UNLIKELY(result < 0)) {
    config_logf_(ctx, "open failure");
    goto ABORT;
  }
  ctx->fd = result;

  ctx->buf = upd_iso_stack(iso, ctx->size);
  if (HEDLEY_UNLIKELY(ctx->buf == NULL)) {
    config_logf_(ctx, "config file buffer allocation failure");

    const bool close =
      0 <= uv_fs_close(&iso->loop, &ctx->fsreq, ctx->fd, config_close_cb_);
    if (HEDLEY_UNLIKELY(!close)) {
      goto ABORT;
    }
    return;
  }

  const uv_buf_t buf = uv_buf_init((char*) ctx->buf, ctx->size);

  const bool read = 0 <= uv_fs_read(
    &iso->loop, &ctx->fsreq, ctx->fd, &buf, 1, 0, config_read_cb_);
  if (HEDLEY_UNLIKELY(!read)) {
    upd_iso_unstack(iso, ctx->buf);
    config_logf_(ctx, "read failure");

    const bool close =
      0 <= uv_fs_close(&iso->loop, &ctx->fsreq, ctx->fd, config_close_cb_);
    if (HEDLEY_UNLIKELY(!close)) {
      goto ABORT;
    }
    return;
  }
  return;

ABORT:
  config_unref_(ctx);
}

static void config_read_cb_(uv_fs_t* req) {
  ctx_t_*    ctx = req->data;
  upd_iso_t* iso = ctx->iso;

  yaml_document_t* doc = &ctx->doc;

  const ssize_t result = req->result;
  uv_fs_req_cleanup(req);

  if (HEDLEY_UNLIKELY(result < 0)) {
    upd_iso_unstack(iso, ctx->buf);
    config_logf_(ctx, "read failure");
    goto EXIT;
  }

  yaml_parser_t parser = {0};
  if (HEDLEY_UNLIKELY(!yaml_parser_initialize(&parser))) {
    upd_iso_unstack(iso, ctx->buf);
    config_logf_(ctx, "yaml parser allocation failure");
    goto EXIT;
  }
  yaml_parser_set_input_string(&parser, ctx->buf, result);

  const bool parse = yaml_parser_load(&parser, doc);
  yaml_parser_delete(&parser);
  upd_iso_unstack(iso, ctx->buf);
  if (HEDLEY_UNLIKELY(!parse)) {
    config_logf_(ctx, "yaml parser error");
    goto EXIT;
  }

  config_parse_(ctx);

  bool close;
EXIT:
  close = 0 <= uv_fs_close(&iso->loop, &ctx->fsreq, ctx->fd, config_close_cb_);
  if (HEDLEY_UNLIKELY(!close)) {
    config_unref_(ctx);
  }
}

static void config_close_cb_(uv_fs_t* req) {
  ctx_t_* ctx = req->data;

  const ssize_t result = req->result;
  uv_fs_req_cleanup(req);

  if (HEDLEY_UNLIKELY(result != 0)) {
    config_logf_(ctx, "close failure");
  }
  config_unref_(ctx);
}
