#include "common.h"


#define CONFIG_FILE_ "upd.yml"

#define CONFIG_FILE_MAX_ (1024*1024*4)  /* = 4 MiB */


typedef struct ctx_t_         ctx_t_;
typedef struct task_t_        task_t_;
typedef struct task_file_t_   task_file_t_;
typedef struct task_server_t_ task_server_t_;

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
};

typedef struct config_field_t_ {
  const char*      name;
  yaml_node_t**    node;
  yaml_node_type_t type;
} config_field_t_;


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

struct task_server_t_ {
  task_t_*     parent;
  yaml_node_t* node;

  const uint8_t* path;
  size_t         pathlen;

  const uint8_t* host;
  size_t         hostlen;

  uint16_t port;
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
config_find_all_fields_(
  ctx_t_*                ctx,
  yaml_node_t*           node,
  const config_field_t_* fields);

static
bool
config_toimax_(
  ctx_t_*      ctx,
  yaml_node_t* node,
  intmax_t*    i);

static
bool
config_tobool_(
  ctx_t_*      ctx,
  yaml_node_t* node,
  bool*        b);

static
bool
config_check_npath_(
  ctx_t_*        ctx,
  const uint8_t* npath);


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
task_parse_server_cb_(
  task_t_* task);


static
void
pkg_install_cb_(
  upd_pkg_install_t* inst);


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


static
void
server_pathfind_cb_(
  upd_pathfind_t* pf);


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

static void config_find_all_fields_(
    ctx_t_* ctx, yaml_node_t* node, const config_field_t_* fields) {
  if (HEDLEY_UNLIKELY(node->type != YAML_MAPPING_NODE)) {
    config_lognf_(ctx, node, "expected mapping node");
    return;
  }

  size_t used = 0;

  yaml_node_pair_t* beg = node->data.mapping.pairs.start;
  yaml_node_pair_t* end = node->data.mapping.pairs.top;
  for (yaml_node_pair_t* itr = beg; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(&ctx->doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(&ctx->doc, itr->value);

    if (HEDLEY_UNLIKELY(key == NULL)) {
      config_lognf_(ctx, node, "found null key");
      continue;
    }
    if (HEDLEY_UNLIKELY(key->type != YAML_SCALAR_NODE)) {
      config_lognf_(ctx, key, "key must be scalar");
      continue;
    }
    if (HEDLEY_UNLIKELY(val == NULL)) {
      config_lognf_(ctx, key, "found null value");
      continue;
    }

    const uint8_t* k    = key->data.scalar.value;
    const size_t   klen = key->data.scalar.length;

    /* check if the key is known */
    const config_field_t_* field = NULL;
    for (const config_field_t_* f = fields; f->name && !field; ++f) {
      const bool match = utf8ncmp(f->name, k, klen) == 0 && f->name[klen] == 0;
      if (HEDLEY_UNLIKELY(match)) {
        field = f;
      }
    }
    if (HEDLEY_UNLIKELY(!field)) {
      config_lognf_(ctx, key, "unknown field");
      continue;
    }
    if (HEDLEY_UNLIKELY(val->type != field->type)) {
      config_lognf_(ctx, key, "incompatible type");
      continue;
    }
    if (HEDLEY_UNLIKELY(*field->node)) {
      config_lognf_(ctx, key, "duplicated field");
      continue;
    }
    *field->node = val;
    ++used;
  }
  if (HEDLEY_UNLIKELY(used != (size_t) (end-beg))) {
    config_lognf_(ctx, node, "some fields in the mapping have no effects");
  }
}

static bool config_toimax_(ctx_t_* ctx, yaml_node_t* node, intmax_t* i) {
  if (HEDLEY_UNLIKELY(node->type != YAML_SCALAR_NODE)) {
    config_lognf_(ctx, node, "expected scalar node");
    return false;
  }
  const uint8_t* v    = node->data.scalar.value;
  const size_t   vlen = node->data.scalar.length;

  char temp[32];
  if (HEDLEY_UNLIKELY(vlen+1 > sizeof(temp))) {
    config_lognf_(ctx, node, "too long integer specification");
    return false;
  }
  utf8ncpy(temp, v, vlen);
  temp[vlen] = 0;

  char* end;
  const intmax_t x = strtoimax(temp, &end, 0);
  if (HEDLEY_UNLIKELY(end != temp+vlen)) {
    config_lognf_(ctx, node, "invalid integer");
    return false;
  }
  if (HEDLEY_UNLIKELY(x == INTMAX_MAX || x == INTMAX_MIN)) {
    config_lognf_(ctx, node, "overflown integer");
    return false;
  }
  *i = x;
  return true;
}

static bool config_tobool_(ctx_t_* ctx, yaml_node_t* node, bool* b) {
  if (!node) {
    return true;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_SCALAR_NODE)) {
    config_lognf_(ctx, node, "expected scalar node");
    return false;
  }

  const uint8_t* v    = node->data.scalar.value;
  const size_t   vlen = node->data.scalar.length;

  if (utf8ncasecmp("true",v, vlen) == 0) {
    *b = true;
    return true;
  }
  if (utf8ncasecmp("false",v, vlen) == 0) {
    *b = false;
    return true;
  }

  config_lognf_(ctx, node, "expected 'true' or 'false'");
  return false;
}

static bool config_check_npath_(ctx_t_* ctx, const uint8_t* npath) {
  const size_t interlen =
    cwk_path_get_intersection((char*) ctx->path, (char*) npath); 
  switch (ctx->path[interlen]) {
  case 0:
    return true;
  case '/':
    return ctx->path[interlen+1] == 0;
  default:
    return false;
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
    } else if (match_("import")) {
      q = task_queue_with_dup_(&(task_t_) {
          .ctx  = ctx,
          .node = val,
          .cb   = task_parse_import_cb_,
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


static void task_parse_require_cb_(task_t_* task) {
  ctx_t_*          ctx  = task->ctx;
  yaml_document_t* doc  = &ctx->doc;
  yaml_node_t*     node = task->node;

  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(!(ctx->load->feats & UPD_CONFIG_REQUIRE))) {
    config_lognf_(ctx, node, "'require' block is not allowed in this context");
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
      config_lognf_(ctx, node, "null item found");
      continue;
    }

    struct {
      yaml_node_t* url;
      yaml_node_t* pkg;
      yaml_node_t* preserve;
      yaml_node_t* verify_ssl;
    } fields = { NULL };
    config_find_all_fields_(ctx, val, (config_field_t_[]) {
        { "url",        &fields.url,        YAML_SCALAR_NODE,   },
        { "pkg",        &fields.pkg,        YAML_SEQUENCE_NODE, },
        { "preserve",   &fields.preserve,   YAML_SCALAR_NODE,   },
        { "verify_ssl", &fields.verify_ssl, YAML_SCALAR_NODE,   },
        { NULL },
      });
    if (HEDLEY_UNLIKELY(!fields.url || !fields.pkg)) {
      config_lognf_(ctx, node, "'url' and 'pkg' fields required");
      continue;
    }

    const uint8_t* url    = fields.url->data.scalar.value;
    const size_t   urllen = fields.url->data.scalar.length;

    bool preserve = false, verify_ssl = false;
    const bool options =
      config_tobool_(ctx, fields.preserve,   &preserve) &&
      config_tobool_(ctx, fields.verify_ssl, &verify_ssl);
    if (HEDLEY_UNLIKELY(!options)) {
      continue;
    }

    yaml_node_item_t* itr = fields.pkg->data.sequence.items.start;
    yaml_node_item_t* end = fields.pkg->data.sequence.items.top;
    for (; itr < end; ++itr) {
      yaml_node_t* val = yaml_document_get_node(doc, *itr);
      if (HEDLEY_UNLIKELY(val == NULL)) {
        config_lognf_(ctx, fields.pkg, "null item found");
        continue;
      }
      if (HEDLEY_UNLIKELY(val->type != YAML_SCALAR_NODE)) {
        config_lognf_(ctx, val, "expected scalar node");
        continue;
      }
      const uint8_t* name    = val->data.scalar.value;
      size_t         namelen = val->data.scalar.length;

      const uint8_t* hash    = name;
      size_t         hashlen = namelen;
      while (hashlen && *hash != '#') {
        ++hash; --hashlen;
      }
      if (hashlen) {
        namelen -= hashlen;
        ++hash; --hashlen;
      }

      upd_pkg_install_t* inst = upd_iso_stack(ctx->iso, sizeof(*inst));
      if (HEDLEY_UNLIKELY(inst == NULL)) {
        config_lognf_(ctx, val, "pkg install context allocation failure");
        break;
      }
      *inst = (upd_pkg_install_t) {
        .iso        = ctx->iso,
        .url        = url,
        .urllen     = urllen,
        .name       = name,
        .namelen    = namelen,
        .hash       = hash,
        .hashlen    = hashlen,
        .verify_ssl = verify_ssl,
        .preserve   = preserve,
        .udata      = task,
        .cb         = pkg_install_cb_,
      };
      ++task->refcnt;
      if (HEDLEY_UNLIKELY(!upd_pkg_install(inst))) {
        task_unref_(task);
        upd_iso_unstack(ctx->iso, inst);
        config_lognf_(ctx, val, "pkg install failure");
        continue;
      }
    }
  }

EXIT:
  task_unref_(task);
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
        .feats = UPD_CONFIG_IMPORTED,
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

    struct {
      yaml_node_t* npath;
      yaml_node_t* driver;
      yaml_node_t* rules;
    } fields = { NULL };
    config_find_all_fields_(ctx, val, (config_field_t_[]) {
        { "npath",  &fields.npath,  YAML_SCALAR_NODE,  },
        { "driver", &fields.driver, YAML_SCALAR_NODE,  },
        { "rules",  &fields.rules,  YAML_MAPPING_NODE, },
        { NULL },
      });

    const upd_driver_t* driver = &upd_driver_syncdir;
    if (fields.driver) {
      const size_t   vlen = fields.driver->data.scalar.length;
      const uint8_t* v    = fields.driver->data.scalar.value;
      driver = upd_driver_lookup(iso, v, vlen);
      if (HEDLEY_UNLIKELY(driver == NULL)) {
        config_lognf_(ctx, fields.driver, "unknown driver");
        continue;
      }
    }
    if (driver == &upd_driver_syncdir) {
      if (HEDLEY_UNLIKELY(!fields.rules)) {
        config_lognf_(ctx, key, "upd.syncdir requires rules");
        continue;
      }
    } else {
      if (HEDLEY_UNLIKELY(fields.rules)) {
        config_lognf_(ctx, key, "rules are ignored");
      }
    }

    const uint8_t* k    = key->data.scalar.value;
    const size_t   klen =
      upd_path_drop_trailing_slash(k, key->data.scalar.length);
    if (HEDLEY_UNLIKELY(klen == 0)) {
      continue;
    }

    size_t         blen = klen;
    const uint8_t* b    = upd_path_basename(k, &blen);
    if (HEDLEY_UNLIKELY(blen == 0)) {
      config_lognf_(ctx, key, "empty path");
      continue;
    }

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
      .driver  = driver,
      .rules   = fields.rules,
    };
    if (HEDLEY_UNLIKELY(fields.npath)) {
      ftask->npath    = fields.npath->data.scalar.value;
      ftask->npathlen = fields.npath->data.scalar.length;
      if (HEDLEY_UNLIKELY(ftask->npathlen >= UPD_PATH_MAX)) {
        upd_iso_unstack(iso, ftask);
        config_lognf_(ctx, key,
          "too long npath: %.*s", (int) ftask->npathlen, ftask->npath);
        goto EXIT;
      }
    }

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

static void task_parse_server_cb_(task_t_* task) {
  ctx_t_*          ctx  = task->ctx;
  upd_iso_t*       iso  = ctx->iso;
  yaml_document_t* doc  = &ctx->doc;
  yaml_node_t*     node = task->node;

  if (HEDLEY_UNLIKELY(node == NULL)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(!(ctx->load->feats & UPD_CONFIG_SERVER))) {
    config_lognf_(ctx, node, "'server' block is not allowed in this context");
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

    struct {
      yaml_node_t* host;
      yaml_node_t* port;
    } fields = { NULL };
    config_find_all_fields_(ctx, val, (config_field_t_[]) {
        { "host", &fields.host, YAML_SCALAR_NODE, },
        { "port", &fields.port, YAML_SCALAR_NODE, },
        { NULL },
      });
    if (HEDLEY_UNLIKELY(!fields.host || !fields.port)) {
      config_lognf_(ctx, key, "'host' and 'port' required");
      continue;
    }

    intmax_t port;
    if (HEDLEY_UNLIKELY(!config_toimax_(ctx, fields.port, &port))) {
      continue;
    }
    if (HEDLEY_UNLIKELY(port <= 0 || UINT16_MAX < port)) {
      config_lognf_(ctx, fields.port, "invalid port number");
      continue;
    }

    const uint8_t* host    = (uint8_t*) "0.0.0.0";
    size_t         hostlen = utf8size_lazy(host);
    if (HEDLEY_LIKELY(fields.host)) {
      host    = fields.host->data.scalar.value;
      hostlen = fields.host->data.scalar.length;
    }

    task_server_t_* srv = upd_iso_stack(iso, sizeof(*srv));
    if (HEDLEY_UNLIKELY(srv == NULL)) {
      config_lognf_(ctx, key,
        "server builder allocation failure, skipping followings");
      break;
    }
    *srv = (task_server_t_) {
      .parent  = task,
      .node    = val,
      .path    = key->data.scalar.value,
      .pathlen = key->data.scalar.length,
      .host    = host,
      .hostlen = hostlen,
      .port    = port,
    };

    ++task->refcnt;
    const bool pf = upd_pathfind_with_dup(&(upd_pathfind_t) {
        .iso   = iso,
        .path  = (uint8_t*) srv->path,
        .len   = srv->pathlen,
        .udata = srv,
        .cb    = server_pathfind_cb_,
      });
    if (HEDLEY_UNLIKELY(!pf)) {
      task_unref_(task);
      upd_iso_unstack(iso, srv);
      config_lognf_(ctx, key, "pathfind context allocation failure");
      continue;
    }
  }

EXIT:
  task_unref_(task);
}


static void pkg_install_cb_(upd_pkg_install_t* inst) {
  task_t_*   task = inst->udata;
  ctx_t_*    ctx  = task->ctx;
  upd_iso_t* iso  = ctx->iso;

  if (HEDLEY_UNLIKELY(inst->state != UPD_PKG_INSTALL_DONE)) {
    config_logf_(ctx,
      "failed to install %.*s", (int) inst->namelen, inst->name);
  }
  upd_iso_unstack(iso, inst);
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

  uint8_t rpath[UPD_PATH_MAX];
  if (ftask->npath != NULL) {
    utf8ncpy(rpath, ftask->npath, ftask->npathlen);
  }
  rpath[ftask->npathlen] = 0;

  uint8_t npath[UPD_PATH_MAX];
  const size_t npathlen = cwk_path_join(
    (char*) ctx->path, (char*) rpath, (char*) npath, UPD_PATH_MAX);
  if (HEDLEY_UNLIKELY(npathlen >= UPD_PATH_MAX)) {
    config_lognf_(ctx, ftask->node, "too long npath");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!config_check_npath_(ctx, npath))) {
    config_lognf_(ctx, ftask->node, "directory traversal detected X<");
    goto ABORT;
  }

  upd_file_t* f = upd_file_new(&(upd_file_t) {
      .iso      = iso,
      .driver   = ftask->driver,
      .npath    = npath,
      .npathlen = npathlen
    });
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


static void server_pathfind_cb_(upd_pathfind_t* pf) {
  task_server_t_* srv  = pf->udata;
  task_t_*        task = srv->parent;
  ctx_t_*         ctx  = task->ctx;
  upd_iso_t*      iso  = ctx->iso;

  upd_file_t* fpro = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(fpro == NULL)) {
    config_lognf_(ctx, srv->node,
      "no such program found: %.*s", (int) srv->pathlen, srv->path);
    goto EXIT;
  }

  uint8_t host[256];
  if (HEDLEY_UNLIKELY(srv->hostlen >= 256)) {
    config_lognf_(ctx, srv->node,
      "too long hostname: %.*s", (int) srv->hostlen, srv->host);
    goto EXIT;
  }
  utf8ncpy(host, srv->host, srv->hostlen);
  host[srv->hostlen] = 0;

  upd_file_t* fsrv = upd_driver_srv_tcp_new(fpro, host, srv->port);
  if (HEDLEY_UNLIKELY(fsrv == NULL)) {
    config_lognf_(ctx, srv->node, "server start failure");
    goto EXIT;
  }
  upd_file_unref(fsrv);

EXIT:
  upd_iso_unstack(iso, srv);
  task_unref_(task);
}
