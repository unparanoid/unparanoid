#include "common.h"


#define CONFIG_FILE_ "upd.yml"

#define CONFIG_FILE_MAX_ (1024*1024*4)  /* = 4 MiB */

#define FILE_PARAM_MAX_ 1024


typedef struct ctx_t_       ctx_t_;
typedef struct task_t_      task_t_;
typedef struct task_file_t_ task_file_t_;

struct ctx_t_ {
  upd_iso_t*         iso;
  upd_config_load_t* load;

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

  /* used in config_create_file_'s recursion */
  uint8_t temp[UPD_PATH_MAX];
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

  const uint8_t* path;
  size_t pathlen;

  const uint8_t* dir;
  size_t dirlen;

  const uint8_t* name;
  size_t namelen;

  const uint8_t* npath;
  size_t npathlen;

  const uint8_t* param;
  size_t         paramlen;

  const upd_driver_t* driver;
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
  ctx_t_*            ctx,
  const yaml_node_t* n,
  const char*        fmt,
  ...);

static
bool
config_check_npath_(
  ctx_t_*        ctx,
  const uint8_t* npath);

static
upd_file_t*
config_create_file_(
  ctx_t_*            ctx,
  const yaml_node_t* node,
  upd_file_t*        proto);


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
task_parse_import_cb_(
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
import_load_cb_(
  upd_config_load_t* load);


static
void
driver_load_cb_(
  upd_driver_load_external_t* load);


static
void
file_pathfind_cb_(
  upd_pathfind_t* pf);

static
void
file_lock_cb_(
  upd_file_lock_t* lock);

static
void
file_add_cb_(
  upd_req_t* req);


bool upd_config_load(upd_config_load_t* load) {
  upd_iso_t*     iso  = load->iso;
  const uint8_t* path = load->path;

  const size_t fpathlen = cwk_path_join((char*) path, CONFIG_FILE_, NULL, 0);

  ctx_t_* ctx = upd_iso_stack(iso, sizeof(*ctx) + (fpathlen+1)*2);
  if (HEDLEY_UNLIKELY(ctx == NULL)) {
    return false;
  }
  *ctx = (ctx_t_) {
    .iso      = iso,
    .load     = load,
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
  upd_iso_t*         iso  = ctx->iso;
  upd_config_load_t* load = ctx->load;

  if (HEDLEY_UNLIKELY(--ctx->refcnt == 0)) {
    yaml_document_delete(&ctx->doc);
    upd_iso_unstack(iso, ctx);
    load->cb(load);
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

static void config_lognf_(
    ctx_t_* ctx, const yaml_node_t* n, const char* fmt, ...) {
  upd_iso_msgf(ctx->iso, "config error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(ctx->iso, fmt, args);
  va_end(args);

  upd_iso_msgf(ctx->iso,
    " (%s:%zu:%zu)\n",
    ctx->fpath, n->start_mark.line+1, n->start_mark.column+1);
}

static bool config_check_npath_(ctx_t_* ctx, const uint8_t* npath) {
  /* TODO: check npath is inside of ctx->path */
  (void) ctx;
  (void) npath;
  return true;
}

static upd_file_t* config_create_file_(
    ctx_t_* ctx, const yaml_node_t* node, upd_file_t* proto) {
  upd_iso_t* iso = ctx->iso;

  proto->iso = ctx->iso;

  switch (node->type) {
  case YAML_SCALAR_NODE: {
    const uint8_t* dname    = node->data.scalar.value;
    const size_t   dnamelen = node->data.scalar.length;

    proto->driver = upd_driver_lookup(iso, dname, dnamelen);
    if (HEDLEY_UNLIKELY(proto->driver == NULL)) {
      config_lognf_(ctx, node,
        "driver '%.*s' not found", (int) dnamelen, dname);
      return NULL;
    }
  } return upd_file_new(proto);

  case YAML_MAPPING_NODE: {
    const yaml_node_t* npath  = NULL;
    const yaml_node_t* param  = NULL;
    const yaml_node_t* driver = NULL;

    const char* invalid =
      upd_yaml_find_fields(&ctx->doc, node, (upd_yaml_field_t[]) {
          { .name = "npath",  .required = false, .str = &npath,  },
          { .name = "param",  .required = false, .str = &param,  },
          { .name = "driver", .required = true,  .str = &driver, },
          { NULL, },
        });
    if (HEDLEY_UNLIKELY(invalid)) {
      config_lognf_(ctx, node, "invalid field '%s'", invalid);
      return NULL;
    }

    uint8_t* npath_abs = ctx->temp;
    if (npath) {
      if (HEDLEY_UNLIKELY(proto->npathlen)) {
        config_lognf_(ctx, npath, "backend file cannot override npath");
        return NULL;
      }

      const uint8_t* npath_   = npath->data.scalar.value;
      const size_t   npathlen = npath->data.scalar.length;

      uint8_t npath_c[UPD_PATH_MAX];
      if (HEDLEY_UNLIKELY(npathlen >= sizeof(npath_c))) {
        config_lognf_(ctx, node, "too long npath");
        return NULL;
      }
      utf8ncpy(npath_c, npath_, npathlen);
      npath_c[npathlen] = 0;

      const size_t abslen =
        cwk_path_join((char*) ctx->path, (char*) npath_c, NULL, 0);
      if (HEDLEY_UNLIKELY(abslen >= sizeof(ctx->temp))) {
        config_lognf_(ctx, node, "too long npath");
        return NULL;
      }
      cwk_path_join(
        (char*) ctx->path, (char*) npath_c, (char*) npath_abs, abslen+1);
      if (HEDLEY_UNLIKELY(!config_check_npath_(ctx, npath_abs))) {
        config_lognf_(ctx, npath, "directory traversal detected X(");
        return NULL;
      }
      proto->npath    = npath_abs;
      proto->npathlen = abslen;
    }

    if (param) {
      proto->param    = param->data.scalar.value;
      proto->paramlen = param->data.scalar.length;
    }
    return config_create_file_(ctx, driver, proto);
  } break;

  case YAML_SEQUENCE_NODE: {
    const yaml_node_item_t* itr = node->data.sequence.items.top;
    const yaml_node_item_t* beg = node->data.sequence.items.start;

    upd_file_t* f = NULL;
    for (;;) {
      if (itr == beg) return f;
      --itr;

      const yaml_node_t* item = yaml_document_get_node(&ctx->doc, *itr);

      proto->backend  = f;
      proto->paramlen = 0;

      upd_file_t* temp = config_create_file_(ctx, item, proto);
      if (f) upd_file_unref(f);

      if (HEDLEY_UNLIKELY(temp == NULL)) {
        if (f) upd_file_unref(f);
        return NULL;
      }
      f = temp;
    }
  } break;

  default:
    config_lognf_(ctx, node, "expected scalar, map, or sequence");
    return NULL;
  }
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

  const bool parsed = upd_yaml_parse(doc, ctx->buf, result);
  upd_iso_unstack(iso, ctx->buf);
  if (HEDLEY_UNLIKELY(!parsed)) {
    config_logf_(ctx, "yaml parse failure");
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
    if (upd_strcaseq_c("import", k, klen)) {
      q = task_queue_with_dup_(&(task_t_) {
          .ctx  = ctx,
          .node = val,
          .cb   = task_parse_import_cb_,
        });
    } else if (upd_strcaseq_c("driver", k, klen)) {
      q = task_queue_with_dup_(&(task_t_) {
          .ctx  = ctx,
          .node = val,
          .cb   = task_parse_driver_cb_,
        });
    } else if (upd_strcaseq_c("file", k, klen)) {
      q = task_queue_with_dup_(&(task_t_) {
          .ctx  = ctx,
          .node = val,
          .cb   = task_parse_file_cb_,
        });
    } else {
      config_lognf_(ctx, key, "unknown block");
      continue;
    }

    if (HEDLEY_UNLIKELY(!q)) {
      config_lognf_(ctx, key, "queuing task failure, aborting");
      break;
    }
  }
  ctx->load->ok = true;

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


static void task_parse_import_cb_(task_t_* task) {
  ctx_t_*          ctx  = task->ctx;
  upd_iso_t*       iso  = ctx->iso;
  yaml_document_t* doc  = &ctx->doc;
  yaml_node_t*     node = task->node;

  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(!(ctx->load->feats & UPD_CONFIG_IMPORT))) {
    config_lognf_(ctx, node, "'import' block is not allowed in this context");
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_SEQUENCE_NODE)) {
    config_lognf_(ctx, node, "'import' block must be sequence node");
    goto EXIT;
  }

  yaml_node_item_t* itr = node->data.sequence.items.start;
  yaml_node_item_t* end = node->data.sequence.items.top;
  for (; itr < end; ++itr) {
    yaml_node_t* val = yaml_document_get_node(doc, *itr);
    if (HEDLEY_UNLIKELY(val == NULL)) {
      config_lognf_(ctx, node, "null item found");
      continue;
    }
    if (HEDLEY_UNLIKELY(val->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, val, "expected scalar");
      continue;
    }
    const uint8_t* v    = val->data.scalar.value;
    const size_t   vlen = val->data.scalar.length;
    if (HEDLEY_UNLIKELY(vlen >= UPD_PATH_MAX)) {
      config_lognf_(ctx, val, "too long path");
      continue;
    }

    uint8_t vtemp[UPD_PATH_MAX];
    utf8ncpy(vtemp, v, vlen);
    vtemp[vlen] = 0;

    uint8_t path[UPD_PATH_MAX];
    const size_t pathlen = cwk_path_get_absolute(
      (char*) ctx->path, (char*) vtemp, (char*) path, UPD_PATH_MAX);
    if (HEDLEY_UNLIKELY(pathlen >= UPD_PATH_MAX)) {
      config_lognf_(ctx, val, "too long path");
      continue;
    }

    ++task->refcnt;
    const bool load = upd_config_load_with_dup(&(upd_config_load_t) {
        .iso   = iso,
        .path  = path,
        .feats = UPD_CONFIG_SECURE,
        .udata = task,
        .cb    = import_load_cb_,
      });
    if (HEDLEY_UNLIKELY(!load)) {
      config_lognf_(ctx, val, "config loader context allocation failure");
      task_unref_(task);
      continue;
    }
  }

EXIT:
  task_unref_(task);
}

static void task_parse_driver_cb_(task_t_* task) {
  ctx_t_*          ctx  = task->ctx;
  upd_iso_t*       iso  = ctx->iso;
  yaml_document_t* doc  = &ctx->doc;
  yaml_node_t*     node = task->node;

  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(!(ctx->load->feats & UPD_CONFIG_DRIVER))) {
    config_lognf_(ctx, node, "'driver' block is not allowed in this context");
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

    if (HEDLEY_UNLIKELY(vlen >= UPD_PATH_MAX)) {
      config_lognf_(ctx, val, "too long path");
      continue;
    }

    uint8_t rpath[UPD_PATH_MAX];
    utf8ncpy(rpath, v, vlen);
    rpath[vlen] = 0;

    const size_t plen =
      cwk_path_join((char*) ctx->path, (char*) rpath, NULL, 0);

    upd_driver_load_external_t* load = upd_iso_stack(iso, sizeof(*load)+plen+1);
    if (HEDLEY_UNLIKELY(load == NULL)) {
      config_lognf_(ctx, val, "external driver loader allocation failure");
      goto EXIT;
    }
    cwk_path_join((char*) ctx->path, (char*) rpath, (char*) (load+1), plen+1);

    if (HEDLEY_UNLIKELY(!config_check_npath_(ctx, (uint8_t*) (load+1)))) {
      upd_iso_unstack(iso, load);
      config_lognf_(ctx, val, "directory traversal detected X<");
      continue;
    }

    *load = (upd_driver_load_external_t) {
      .iso      = iso,
      .npath    = (uint8_t*) (load+1),
      .npathlen = plen,
      .udata    = task,
      .cb       = driver_load_cb_,
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
  ctx_t_*          ctx  = task->ctx;
  upd_iso_t*       iso  = ctx->iso;
  yaml_document_t* doc  = &ctx->doc;
  yaml_node_t*     node = task->node;

  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(!(ctx->load->feats & UPD_CONFIG_FILE))) {
    config_lognf_(ctx, node, "'file' block is not allowed in this context");
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

    const uint8_t* path    = key->data.scalar.value;
    const size_t   pathlen =
      upd_path_drop_trailing_slash(path, key->data.scalar.length);
    if (HEDLEY_UNLIKELY(pathlen == 0)) {
      config_lognf_(ctx, key, "empty path");
      continue;
    }
    if (HEDLEY_UNLIKELY(path[0] != '/')) {
      config_lognf_(ctx, key, "path must start with '/'");
      continue;
    }

    size_t namelen = pathlen;
    const uint8_t* name = upd_path_basename(path, &namelen);
    if (HEDLEY_UNLIKELY(namelen == 0)) {
      config_lognf_(ctx, key, "empty path");
    }

    task_file_t_* ftask = upd_iso_stack(iso, sizeof(*ftask));
    if (HEDLEY_UNLIKELY(ftask == NULL)) {
      config_lognf_(ctx, key, "task allocation failure");
      goto EXIT;
    }
    *ftask = (task_file_t_) {
      .parent   = task,
      .node     = val,
      .path     = path,
      .pathlen  = pathlen,
      .dir      = path,
      .dirlen   = upd_path_dirname(path, pathlen),
      .name     = name,
      .namelen  = namelen,
    };

    ++task->refcnt;
    const bool pf = upd_pathfind_with_dup(&(upd_pathfind_t) {
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


static void import_load_cb_(upd_config_load_t* load) {
  task_t_*   task = load->udata;
  ctx_t_*    ctx  = task->ctx;
  upd_iso_t* iso  = ctx->iso;
  upd_iso_unstack(iso, load);

  task_unref_(task);
}


static void driver_load_cb_(upd_driver_load_external_t* load) {
  task_t_*   task = load->udata;
  ctx_t_*    ctx  = task->ctx;
  upd_iso_t* iso  = ctx->iso;

  if (HEDLEY_UNLIKELY(!load->ok)) {
    config_logf_(ctx, "failed to load external driver: %s", load->npath);
  }
  upd_iso_unstack(iso, load);
  task_unref_(task);
}


static void file_pathfind_cb_(upd_pathfind_t* pf) {
  task_file_t_* ftask = pf->udata;
  task_t_*      task  = ftask->parent;
  ctx_t_*       ctx   = task->ctx;
  upd_iso_t*    iso   = ctx->iso;

  upd_file_t* f = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(f == NULL)) {
    config_lognf_(ctx, ftask->node, "failed to build fs tree");
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

  uint8_t path[UPD_PATH_MAX+1];
  path[0] = '/';
  utf8ncpy(path+1, ftask->path, ftask->pathlen);
  const size_t pathlen = upd_path_normalize(path, ftask->pathlen+1);

  upd_file_t* f = config_create_file_(ctx, ftask->node, &(upd_file_t) {
      .path    = path,
      .pathlen = pathlen,
    });
  if (HEDLEY_UNLIKELY(f == NULL)) {
    config_lognf_(ctx, ftask->node, "file creation failure");
    goto ABORT;
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
