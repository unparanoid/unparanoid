#include "common.h"


#define CONFIG_FILE_ "upd.yml"

#define CONFIG_FILE_MAX_ (1024*1024*4)  /* = 4 MiB */


typedef struct ctx_t_       ctx_t_;
typedef struct task_t_      task_t_;
typedef struct task_file_t_ task_file_t_;

struct ctx_t_ {
  upd_iso_t* iso;

  uint8_t* path;
  size_t   pathlen;
  uint8_t* fpath;
  size_t   fpathlen;

  uv_file  fd;
  uint8_t* buf;
  size_t   size;

  uv_fs_t fsreq;

  yaml_document_t doc;
  size_t          refcnt;

  task_t_* last_task;
};

struct task_t_ {
  ctx_t_*  ctx;
  task_t_* next;

  yaml_node_t* node;

  size_t refcnt;

  void
  (*cb)(
    task_t_* task);
};

struct task_file_t_ {
  task_t_*     parent;
  yaml_node_t* node;

  const uint8_t* dir;
  size_t dirlen;

  const uint8_t* name;
  size_t namelen;

  const uint8_t* npath;
  size_t npathlen;

  const upd_driver_t* driver;
  yaml_node_t*        rules;
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
bool
task_queue_with_dup_(
  const task_t_* task);

static
void
task_unref_(
  task_t_* task);


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


static
void
task_parse_require_cb_(
  task_t_* task);

static
void
task_parse_driver_cb_(
  task_t_* task);

static
void
task_parse_file_cb_(
  task_t_* task);

static
void
task_parse_server_cb_(
  task_t_* task);


static
void
driver_load_cb_(
  upd_driver_load_external_t* load);


static
void
file_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
file_lock_cb_(
  upd_file_lock_t* lock);

static
void
file_add_cb_(
  upd_req_t* req);


static
void
server_build_cb_(
  upd_srv_build_t* b);


bool upd_config_load_from_path(upd_iso_t* iso, const uint8_t* path) {
  /* Adds a surplus of 8 bytes for path join. */
  const size_t fpathlen = cwk_path_join((char*) path, CONFIG_FILE_, NULL, 0);

  ctx_t_* ctx = upd_iso_stack(iso, sizeof(*ctx) + (fpathlen+1)*2);
  if (HEDLEY_UNLIKELY(ctx == NULL)) {
    return false;
  }
  *ctx = (ctx_t_) {
    .iso      = iso,
    .path     = (uint8_t*) (ctx+1) + fpathlen+1,
    .fpath    = (uint8_t*) (ctx+1),
    .fpathlen = fpathlen,
    .fsreq    = { .data = ctx, },
    .refcnt   = 1,
  };
  cwk_path_join((char*) path, CONFIG_FILE_, (char*) ctx->fpath, fpathlen+1);

  cwk_path_get_dirname((char*) ctx->fpath, &ctx->pathlen);
  utf8ncpy(ctx->path, ctx->fpath, ctx->pathlen);
  ctx->path[ctx->pathlen] = 0;

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
    " (%s:%zu:%zu)\n",
    ctx->fpath, n->start_mark.line+1, n->start_mark.column+1);
}


static bool task_queue_with_dup_(const task_t_* src) {
  ctx_t_*    ctx = src->ctx;
  upd_iso_t* iso = ctx->iso;

  task_t_* task = upd_iso_stack(iso, sizeof(*task));
  if (HEDLEY_UNLIKELY(task == NULL)) {
    return false;
  }
  *task = *src;

  task->refcnt = 1;

  task_t_* p = ctx->last_task;
  ctx->last_task = task;
  if (HEDLEY_UNLIKELY(p)) {
    p->next = task;
  } else {
    ++ctx->refcnt;
    task->cb(task);
  }
  return true;
}

static void task_unref_(task_t_* task) {
  assert(task->refcnt);

  ctx_t_*    ctx = task->ctx;
  upd_iso_t* iso = ctx->iso;

  if (HEDLEY_UNLIKELY(--task->refcnt == 0)) {
    if (HEDLEY_UNLIKELY(ctx->last_task == task)) {
      assert(task->next == NULL);
      ctx->last_task = NULL;
      config_unref_(ctx);
    } else {
      assert(task->next != NULL);
      task->next->cb(task->next);
    }
    upd_iso_unstack(iso, task);
  }
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

  yaml_node_t* root = yaml_document_get_root_node(&ctx->doc);
  if (HEDLEY_UNLIKELY(root->type != YAML_MAPPING_NODE)) {
    config_logf_(ctx, "yaml root is not mapping");
    goto EXIT;
  }

  yaml_node_pair_t* itr = root->data.mapping.pairs.start;
  yaml_node_pair_t* end = root->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(doc, itr->value);
    if (HEDLEY_UNLIKELY(key == NULL)) {
      config_lognf_(ctx, root, "yaml fatal error");
      continue;
    }
    if (HEDLEY_UNLIKELY(key->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, key, "key must be scalar");
      continue;
    }

    const uint8_t* k    = key->data.scalar.value;
    const size_t   klen = key->data.scalar.length;

    bool q = false;
#   define match_(v) (sizeof(v) == klen+1 && utf8ncmp(v, k, klen) == 0)
    if (match_("require")) {
      q = task_queue_with_dup_(&(task_t_) {
          .ctx  = ctx,
          .node = val,
          .cb   = task_parse_require_cb_,
        });
    } else if (match_("driver")) {
      q = task_queue_with_dup_(&(task_t_) {
          .ctx  = ctx,
          .node = val,
          .cb   = task_parse_driver_cb_,
        });
    } else if (match_("file")) {
      q = task_queue_with_dup_(&(task_t_) {
          .ctx  = ctx,
          .node = val,
          .cb   = task_parse_file_cb_,
        });
    } else if (match_("server")) {
      q = task_queue_with_dup_(&(task_t_) {
          .ctx  = ctx,
          .node = val,
          .cb   = task_parse_server_cb_,
        });
    } else {
      config_lognf_(ctx, key, "unknown block");
      continue;
    }
#   undef match_

    if (HEDLEY_UNLIKELY(!q)) {
      config_lognf_(ctx, key, "queuing task failure, aborting");
      break;
    }
  }

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


static void task_parse_require_cb_(task_t_* task) {
  ctx_t_*          ctx = task->ctx;
  yaml_document_t* doc = &ctx->doc;

  yaml_node_t* node = task->node;
  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_SEQUENCE_NODE)) {
    config_lognf_(ctx, node, "'require' block must be sequence node");
    goto EXIT;
  }

  yaml_node_item_t* itr = node->data.sequence.items.start;
  yaml_node_item_t* end = node->data.sequence.items.top;
  for (; itr < end; ++itr) {
    yaml_node_t* val = yaml_document_get_node(doc, *itr);
    if (HEDLEY_UNLIKELY(val == NULL)) {
      continue;
    }
    if (HEDLEY_UNLIKELY(val->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, val, "scalar expected");
      continue;
    }

    const size_t   vlen = val->data.scalar.length;
    const uint8_t* v    = val->data.scalar.value;
    config_lognf_(ctx, val, "require: %.*s", (int) vlen, v);
    /* TODO */
  }

EXIT:
  task_unref_(task);
}

static void task_parse_driver_cb_(task_t_* task) {
  ctx_t_*          ctx = task->ctx;
  upd_iso_t*       iso = ctx->iso;
  yaml_document_t* doc = &ctx->doc;

  yaml_node_t* node = task->node;
  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_SEQUENCE_NODE)) {
    config_lognf_(ctx, node, "'driver' block must be sequence node");
    goto EXIT;
  }

  yaml_node_item_t* itr = node->data.sequence.items.start;
  yaml_node_item_t* end = node->data.sequence.items.top;
  for (; itr < end; ++itr) {
    yaml_node_t* val = yaml_document_get_node(doc, *itr);
    if (HEDLEY_UNLIKELY(val == NULL)) {
      continue;
    }
    if (HEDLEY_UNLIKELY(val->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, val, "scalar expected");
      continue;
    }

    const size_t   vlen = val->data.scalar.length;
    const uint8_t* v    = val->data.scalar.value;

    uint8_t* rpath = upd_iso_stack(iso, vlen+1);
    if (HEDLEY_UNLIKELY(rpath == NULL)) {
      config_lognf_(ctx, val, "temp memory allocation failure");
      goto EXIT;
    }
    utf8ncpy(rpath, v, vlen);
    rpath[vlen] = 0;

    if (HEDLEY_UNLIKELY(cwk_path_is_absolute((char*) rpath))) {
      upd_iso_unstack(iso, rpath);
      config_lognf_(ctx, val, "absolute path is forbidden");
      continue;
    }

    const size_t plen =
      cwk_path_join((char*) ctx->path, (char*) rpath, NULL, 0);

    upd_driver_load_external_t* load = upd_iso_stack(iso, sizeof(*load)+plen+1);
    if (HEDLEY_UNLIKELY(load == NULL)) {
      upd_iso_unstack(iso, rpath);
      config_lognf_(ctx, val, "external driver loader allocation failure");
      goto EXIT;
    }
    cwk_path_join((char*) ctx->path, (char*) rpath, (char*) (load+1), plen+1);
    upd_iso_unstack(iso, rpath);

    *load = (upd_driver_load_external_t) {
      .iso   = iso,
      .npath = (uint8_t*) (load+1),
      .len   = plen,
      .udata = task,
      .cb    = driver_load_cb_,
    };
    ++task->refcnt;
    if (HEDLEY_UNLIKELY(!upd_driver_load_external(load))) {
      task_unref_(task);
      upd_iso_unstack(iso, load);
      continue;
    }
  }

EXIT:
  task_unref_(task);
}

static void task_parse_file_cb_(task_t_* task) {
  ctx_t_*          ctx = task->ctx;
  upd_iso_t*       iso = ctx->iso;
  yaml_document_t* doc = &ctx->doc;

  yaml_node_t* node = task->node;
  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_MAPPING_NODE)) {
    config_lognf_(ctx, node, "'file' block must be mapping node");
    goto EXIT;
  }

  yaml_node_pair_t* itr = node->data.mapping.pairs.start;
  yaml_node_pair_t* end = node->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(doc, itr->value);
    if (HEDLEY_UNLIKELY(key == NULL || val == NULL)) {
      config_lognf_(ctx, node, "yaml fatal error");
      continue;
    }
    if (HEDLEY_UNLIKELY(key->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, key, "key must be scalar");
      continue;
    }
    if (HEDLEY_UNLIKELY(val->type != YAML_MAPPING_NODE)) {
      config_lognf_(ctx, val, "value must be mapping");
      continue;
    }

    const uint8_t* k    = key->data.scalar.value;
    const size_t   klen =
      upd_path_drop_trailing_slash(k, key->data.scalar.length);
    if (HEDLEY_UNLIKELY(klen == 0)) {
      continue;
    }

    size_t         blen = klen;
    const uint8_t* b    = upd_path_basename(k, &blen);

    task_file_t_* ftask = upd_iso_stack(iso, sizeof(*ftask));
    if (HEDLEY_UNLIKELY(ftask == NULL)) {
      config_lognf_(ctx, key, "task allocation failure");
      goto EXIT;
    }
    *ftask = (task_file_t_) {
      .parent  = task,
      .node    = val,
      .dir     = k,
      .dirlen  = upd_path_dirname(k, klen),
      .name    = b,
      .namelen = blen,
    };

    yaml_node_pair_t* itr = val->data.mapping.pairs.start;
    yaml_node_pair_t* end = val->data.mapping.pairs.top;
    for (; itr < end; ++itr) {
      yaml_node_t* key = yaml_document_get_node(doc, itr->key);
      yaml_node_t* val = yaml_document_get_node(doc, itr->value);
      if (HEDLEY_UNLIKELY(key == NULL || val == NULL)) {
        config_lognf_(ctx, ftask->node, "yaml fatal error");
        continue;
      }
      if (HEDLEY_UNLIKELY(key->type != YAML_SCALAR_NODE)) {
        config_lognf_(ctx, key, "key must be scalar");
        continue;
      }

      const size_t   klen = key->data.scalar.length;
      const uint8_t* k    = key->data.scalar.value;

#     define match_(v) (sizeof(v) == klen+1 && utf8ncmp(k, v, klen) == 0)
      if (match_("npath")) {
        if (HEDLEY_UNLIKELY(val->type != YAML_SCALAR_NODE)) {
          config_lognf_(ctx, val, "value must be scalar");
          continue;
        }
        ftask->npath    = val->data.scalar.value;
        ftask->npathlen = val->data.scalar.length;

      } else if (match_("driver")) {
        if (HEDLEY_UNLIKELY(val->type != YAML_SCALAR_NODE)) {
          config_lognf_(ctx, val, "value must be scalar");
          continue;
        }
        ftask->driver = upd_driver_lookup(
          iso, val->data.scalar.value, val->data.scalar.length);
        if (HEDLEY_UNLIKELY(ftask->driver == NULL)) {
          config_lognf_(ctx, val, "unknown driver");
          continue;
        }

      } else if (match_("rules")) {
        if (HEDLEY_UNLIKELY(val->type != YAML_MAPPING_NODE)) {
          config_lognf_(ctx, val, "value must be mapping");
          continue;
        }
        ftask->rules = val;

      } else {
        config_lognf_(ctx, key, "unknown key: %.*s", (int) klen, k);
        continue;
      }
#     undef match_
    }

    if (ftask->driver == NULL) {
      ftask->driver = &upd_driver_syncdir;
    }

    if (HEDLEY_UNLIKELY(ftask->namelen == 0)) {
      upd_iso_unstack(iso, ftask);
      config_lognf_(ctx, key, "empty path");
      continue;
    }
    if (HEDLEY_UNLIKELY(ftask->npathlen == 0)) {
      upd_iso_unstack(iso, ftask);
      config_lognf_(ctx, key, "empty npath");
      continue;
    }
    if (ftask->rules) {
      if (HEDLEY_UNLIKELY(ftask->driver != &upd_driver_syncdir)) {
        config_lognf_(ctx, key, "rules is ignored");
      }
    } else {
      if (HEDLEY_UNLIKELY(ftask->driver == &upd_driver_syncdir)) {
        config_lognf_(ctx, key, "rules must specified");
      }
    }

    ++task->refcnt;
    const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
        .iso    = iso,
        .path   = (uint8_t*) ftask->dir,
        .len    = ftask->dirlen,
        .create = true,
        .udata  = ftask,
        .cb     = file_pathfind_cb_,
      });
    if (HEDLEY_UNLIKELY(!pf)) {
      task_unref_(task);
      upd_iso_unstack(iso, ftask);
      config_lognf_(ctx, ftask->node, "pathfind failure");
      continue;
    }
  }

EXIT:
  task_unref_(task);
}

static void task_parse_server_cb_(task_t_* task) {
  ctx_t_*          ctx = task->ctx;
  upd_iso_t* iso = ctx->iso;
  yaml_document_t* doc = &ctx->doc;

  yaml_node_t* node = task->node;
  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_MAPPING_NODE)) {
    config_lognf_(ctx, node, "'server' block must be mapping node");
    goto EXIT;
  }

  yaml_node_pair_t* itr = node->data.mapping.pairs.start;
  yaml_node_pair_t* end = node->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(doc, itr->value);
    if (HEDLEY_UNLIKELY(key == NULL || val == NULL)) {
      config_lognf_(ctx, node, "yaml fatal error");
      continue;
    }
    if (HEDLEY_UNLIKELY(key->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, key, "key must be scalar");
      continue;
    }
    if (HEDLEY_UNLIKELY(val->type != YAML_MAPPING_NODE)) {
      config_lognf_(ctx, key, "value must be mapping");
      continue;
    }

    upd_srv_build_t* b = upd_iso_stack(iso, sizeof(*b));
    if (HEDLEY_UNLIKELY(b == NULL)) {
      config_lognf_(ctx, key,
        "server builder allocation failure, skipping followings");
      break;
    }
    *b = (upd_srv_build_t) {
      .iso     = iso,
      .path    = key->data.scalar.value,
      .pathlen = key->data.scalar.length,
      .udata   = task,
      .cb      = server_build_cb_,
    };

    yaml_node_pair_t* itr = val->data.mapping.pairs.start;
    yaml_node_pair_t* end = val->data.mapping.pairs.top;
    for (; itr < end; ++itr) {
      yaml_node_t* key = yaml_document_get_node(doc, itr->key);
      yaml_node_t* val = yaml_document_get_node(doc, itr->value);
      if (HEDLEY_UNLIKELY(key == NULL || val == NULL)) {
        config_lognf_(ctx, node, "yaml fatal error");
        continue;
      }
      if (HEDLEY_UNLIKELY(key->type != YAML_SCALAR_NODE)) {
        config_lognf_(ctx, key, "key must be scalar");
        continue;
      }
      if (HEDLEY_UNLIKELY(val->type != YAML_SCALAR_NODE)) {
        config_lognf_(ctx, val, "value must be scalar");
        continue;
      }

      const size_t   klen = key->data.scalar.length;
      const uint8_t* k    = key->data.scalar.value;
      const size_t   vlen = val->data.scalar.length;
      const uint8_t* v    = val->data.scalar.value;

#     define match_(v) (sizeof(v) == klen+1 && utf8ncmp(k, v, klen) == 0)
      if (match_("host")) {
        b->host    = v;
        b->hostlen = vlen;

      } else if (match_("port")) {
        char temp[32];
        if (HEDLEY_UNLIKELY(vlen+1 > sizeof(temp))) {
          config_lognf_(ctx, val, "too long port number specification");
          continue;
        }
        utf8ncpy(temp, v, vlen);
        temp[vlen] = 0;

        char* end;
        const uintmax_t x = strtoumax(temp, &end, 0);
        if (HEDLEY_UNLIKELY(end != temp+vlen)) {
          config_lognf_(ctx, val, "invalid port number");
          continue;
        }
        if (HEDLEY_UNLIKELY(x <= 0 || UINT16_MAX < x)) {
          config_lognf_(ctx, val, "port number out of range");
          continue;
        }
        b->port = x;

      } else {
        config_lognf_(ctx, key, "unknown key: %.*s", (int) klen, k);
        continue;
      }
#     undef match_
    }

    ++task->refcnt;
    if (HEDLEY_UNLIKELY(!upd_srv_build(b))) {
      task_unref_(task);
      upd_iso_unstack(iso, b);
      config_lognf_(ctx, key, "building server failure");
      continue;
    }
  }

EXIT:
  task_unref_(task);
}


static void driver_load_cb_(upd_driver_load_external_t* load) {
  task_t_*   task = load->udata;
  ctx_t_*    ctx  = task->ctx;
  upd_iso_t* iso  = ctx->iso;

  if (HEDLEY_UNLIKELY(!load->ok)) {
    config_logf_(ctx,
      "failed to load external driver: %.*s", (int) load->len, load->npath);
  }
  upd_iso_unstack(iso, load);
  task_unref_(task);
}


static void file_pathfind_cb_(upd_req_pathfind_t* pf) {
  task_file_t_* ftask = pf->udata;
  task_t_*      task  = ftask->parent;
  ctx_t_*       ctx   = task->ctx;
  upd_iso_t*    iso   = ctx->iso;

  upd_file_t* f = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(f == NULL)) {
    config_lognf_(ctx, ftask->node, "fs tree building failure");
    goto ABORT;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .udata = ftask,
      .cb    = file_lock_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    config_lognf_(ctx, ftask->node, "lock allocation failure while adding file");
    goto ABORT;
  }
  return;

ABORT:
  upd_iso_unstack(iso, ftask);
  task_unref_(task);
}

static void file_lock_cb_(upd_file_lock_t* lock) {
  task_file_t_* ftask = lock->udata;
  task_t_*      task  = ftask->parent;
  ctx_t_*       ctx   = task->ctx;
  upd_iso_t*    iso   = ctx->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    config_lognf_(ctx, ftask->node, "lock failure while adding file");
    goto ABORT;
  }

  upd_file_t* f = upd_file_new_from_npath(
    iso, ftask->driver, ftask->npath, ftask->npathlen);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    config_lognf_(ctx, ftask->node, "file creation failure");
    goto ABORT;
  }
  if (f->driver == &upd_driver_syncdir) {
    upd_array_of(const upd_driver_rule_t*) rules = {0};

    yaml_node_pair_t* itr = ftask->rules->data.mapping.pairs.start;
    yaml_node_pair_t* end = ftask->rules->data.mapping.pairs.top;
    for (; itr < end; ++itr) {
      yaml_node_t* key = yaml_document_get_node(&ctx->doc, itr->key);
      yaml_node_t* val = yaml_document_get_node(&ctx->doc, itr->value);
      if (HEDLEY_UNLIKELY(key == NULL || key == NULL)) {
        config_lognf_(ctx, ftask->node, "yaml fatal error");
        continue;
      }
      if (HEDLEY_UNLIKELY(key->type != YAML_SCALAR_NODE)) {
        config_lognf_(ctx, key, "key must be scalar");
        continue;
      }
      if (HEDLEY_UNLIKELY(val->type != YAML_SCALAR_NODE)) {
        config_lognf_(ctx, val, "value must be scalar");
        continue;
      }

      const upd_driver_t* d = upd_driver_lookup(
        iso, val->data.scalar.value, val->data.scalar.length);
      if (HEDLEY_UNLIKELY(d == NULL)) {
        config_lognf_(ctx, val, "unknown driver");
        continue;
      }

      const size_t klen = key->data.scalar.length;

      upd_driver_rule_t* rule = NULL;
      if (HEDLEY_UNLIKELY(!upd_malloc(&rule, sizeof(*rule)+klen+1))) {
        config_lognf_(ctx, key, "rule allocation failure");
        break;
      }
      *rule = (upd_driver_rule_t) {
        .ext    = utf8ncpy(rule+1, key->data.scalar.value, klen),
        .len    = klen,
        .driver = d,
      };
      rule->ext[klen] = 0;

      if (HEDLEY_UNLIKELY(!upd_array_insert(&rules, rule, SIZE_MAX))) {
        upd_free(&rule);
        config_lognf_(ctx, key,
          "rule insertion failure, skipping following rules");
        break;
      }
    }
    /* ownership of the rules moves to the driver */
    upd_driver_syncdir_set_rules(f, &rules);
  }

  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file  = lock->file,
      .type  = UPD_REQ_DIR_ADD,
      .dir   = { .entry = {
        .name = (uint8_t*) ftask->name,
        .len  = ftask->namelen,
        .file = f,
      }, },
      .udata = lock,
      .cb    = file_add_cb_,
    });
  upd_file_unref(f);
  if (HEDLEY_UNLIKELY(!add)) {
    config_lognf_(ctx, ftask->node, "add req refused");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  upd_iso_unstack(iso, ftask);

  task_unref_(task);
}

static void file_add_cb_(upd_req_t* req) {
  upd_file_lock_t* lock  = req->udata;
  task_file_t_*    ftask = lock->udata;
  task_t_*         task  = ftask->parent;
  ctx_t_*          ctx   = task->ctx;
  upd_iso_t*       iso   = ctx->iso;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    config_lognf_(ctx, ftask->node, "add req failure");
  }

  upd_iso_unstack(iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  upd_iso_unstack(iso, ftask);

  task_unref_(task);
}


static void server_build_cb_(upd_srv_build_t* b) {
  task_t_*   task = b->udata;
  ctx_t_*    ctx  = task->ctx;
  upd_iso_t* iso  = ctx->iso;

  if (HEDLEY_UNLIKELY(b->srv == NULL)) {
    config_lognf_(ctx, task->node, "failed to build server for port %"PRIu16, b->port);
  }
  upd_iso_unstack(iso, b);
  task_unref_(task);
}
