#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <hedley.h>
#include <msgpack.h>
#include <utf8.h>
#include <yaml.h>

#define UPD_EXTERNAL_DRIVER
#include <libupd.h>
#undef UPD_EXTERNAL_DRIVER

#include <libupd/array.h>
#include <libupd/memory.h>
#include <libupd/msgpack.h>
#include <libupd/str.h>


#define LOG_PREFIX_ "upd.yaml: "
#define MAX_DEPTH_  256


static const upd_driver_t prog_driver_;

upd_external_t upd = {
  .ver = UPD_VER,
  .drivers = (const upd_driver_t*[]) {
    &prog_driver_,
    NULL,
  },
};


typedef struct prog_t_ {
  upd_file_watch_t watch;

  yaml_document_t doc;

  size_t refcnt;

  unsigned clean : 1;
} prog_t_;

typedef struct stream_t_ {
  upd_file_t* file;

  upd_file_t* target;

  upd_file_t*     bin;
  upd_file_lock_t lock;

  upd_msgpack_t             mpk;
  msgpack_unpacked*         upkd;
  const msgpack_object_map* param;

  unsigned broken : 1;
  unsigned locked : 1;
  unsigned busy   : 1;
} stream_t_;


static
bool
prog_init_(
  upd_file_t* f);

static
void
prog_deinit_(
  upd_file_t* f);

static
bool
prog_handle_(
  upd_req_t* req);

static const upd_driver_t prog_driver_ = {
  .name = (uint8_t*) "upd.yaml",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_PROG,
    0,
  },
  .init   = prog_init_,
  .deinit = prog_deinit_,
  .handle = prog_handle_,
};


static
bool
stream_init_(
  upd_file_t* f);

static
void
stream_deinit_(
  upd_file_t* f);

static
bool
stream_handle_(
  upd_req_t* req);

static
void
stream_handle_next_(
  upd_file_t* f);

static
void
stream_handle_get_(
  upd_file_t* f);

static
void
stream_unlock_(
  upd_file_t* f);

static
void
stream_return_(
  upd_file_t* f,
  bool        success,
  const char* msg);

static
void
stream_pack_yaml_(
  upd_file_t*  f,
  yaml_node_t* node,
  size_t       depth);

static
void
stream_pack_scalar_(
  upd_file_t*        f,
  const yaml_node_t* node);

static const upd_driver_t stream_driver_ = {
  .name = (uint8_t*) "upd.yaml.stream_",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_STREAM,
    0,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


static
yaml_node_t*
yaml_find_(
  yaml_document_t*      doc,
  yaml_node_t*          node,
  const msgpack_object* term);


static
void
prog_watch_bin_cb_(
  upd_file_watch_t* w);


static
void
stream_lock_bin_cb_(
  upd_file_lock_t* k);

static
void
stream_read_bin_cb_(
  upd_req_t* req);


static bool prog_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  if (HEDLEY_UNLIKELY(f->npath == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"empty npath\n");
    return false;
  }
  if (HEDLEY_UNLIKELY(f->backend == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"requires backend file\n");
    return false;
  }

  prog_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    upd_iso_msgf(iso, LOG_PREFIX_"context allocation failure\n");
    return false;
  }
  *ctx = (prog_t_) {0};

  ctx->watch = (upd_file_watch_t) {
    .file  = f->backend,
    .udata = f,
    .cb    = prog_watch_bin_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    upd_iso_msgf(iso, LOG_PREFIX_"backend watch refused\n");
    return false;
  }
  f->ctx      = ctx;
  f->mimetype = (uint8_t*) "upd/prog-msgpack;interfaces=object";
  return true;
}

static void prog_deinit_(upd_file_t* f) {
  prog_t_* ctx = f->ctx;

  upd_file_unwatch(&ctx->watch);
  yaml_document_delete(&ctx->doc);

  upd_free(&ctx);
}

static bool prog_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;
  prog_t_*    ctx = f->ctx;
  (void) ctx;

  switch (req->type) {
  case UPD_REQ_PROG_EXEC: {
    upd_file_t* stf = upd_file_new(&(upd_file_t) {
        .iso    = iso,
        .driver = &stream_driver_,
      });
    if (HEDLEY_UNLIKELY(stf == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }

    stream_t_* stctx = stf->ctx;
    stctx->bin    = f->backend;
    stctx->target = f;
    upd_file_ref(stctx->bin);
    upd_file_ref(f);

    req->prog.exec = stf;
    req->result    = UPD_REQ_OK;
    req->cb(req);
    upd_file_unref(stf);
  } return true;

  default:
    req->file = f->backend;
    return upd_req(req);
  }
  HEDLEY_UNREACHABLE();
}


static bool stream_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  stream_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    upd_iso_msgf(iso, LOG_PREFIX_"stream context allocation failure\n");
    return false;
  }
  *ctx = (stream_t_) {
    .mpk = {
      .iso = iso,
    },
  };

  if (HEDLEY_UNLIKELY(!upd_msgpack_init(&ctx->mpk))) {
    upd_iso_msgf(iso, LOG_PREFIX_"msgpack allocation failure\n");
    upd_free(&ctx);
    return false;
  }
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->locked)) {
    stream_unlock_(f);
  }
  upd_msgpack_deinit(&ctx->mpk);

  upd_file_unref(ctx->bin);
  upd_file_unref(ctx->target);
  upd_free(&ctx);
}

static bool stream_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  switch (req->type) {
  case UPD_REQ_DSTREAM_WRITE:
    if (HEDLEY_UNLIKELY(!upd_msgpack_unpack(&ctx->mpk, &req->stream.io))) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    if (HEDLEY_UNLIKELY(!ctx->busy)) {
      stream_handle_next_(f);
    }
    req->result = UPD_REQ_OK;
    req->cb(req);
    return true;

  case UPD_REQ_DSTREAM_READ:
    req->stream.io = (upd_req_stream_io_t) {
      .buf  = (uint8_t*) ctx->mpk.out.data,
      .size = ctx->mpk.out.size,
    };
    uint8_t* ptr = (void*) msgpack_sbuffer_release(&ctx->mpk.out);
    req->result = UPD_REQ_OK;
    req->cb(req);
    free(ptr);
    return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static void stream_handle_next_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  stream_t_* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->upkd != NULL)) {
    msgpack_unpacked_destroy(ctx->upkd);
    upd_iso_unstack(iso, ctx->upkd);
    ctx->upkd = NULL;
  }

  msgpack_unpacked* upkd = upd_msgpack_pop(&ctx->mpk);
  if (HEDLEY_UNLIKELY(upkd == NULL)) {
    ctx->busy = false;
    upd_file_unref(f);
    return;
  }
  if (HEDLEY_LIKELY(!ctx->busy)) {
    ctx->busy = true;
    upd_file_ref(f);
  }
  ctx->upkd = upkd;

  const msgpack_object_str* iface = NULL;
  const msgpack_object_str* cmd   = NULL;

  ctx->param = NULL;
  if (HEDLEY_UNLIKELY(upkd->data.type != MSGPACK_OBJECT_MAP)) {
    stream_return_(f, false, "root must be a map");
    return;
  }

  upd_msgpack_find_fields(&upkd->data.via.map, (upd_msgpack_field_t[]) {
      { .name = "interface", .str = &iface,      },
      { .name = "command",   .str = &cmd,        },
      { .name = "param",     .map = &ctx->param, },
      { NULL, },
    });

  if (iface) {
    if (HEDLEY_UNLIKELY(!upd_streq_c("object", iface->ptr, iface->size))) {
      stream_return_(f, false, "unknown interface");
      return;
    }
  }
  if (HEDLEY_UNLIKELY(!cmd)) {
    stream_return_(f, false, "command not specified");
    return;
  }

  if (upd_streq_c("lock", cmd->ptr, cmd->size)) {
    if (HEDLEY_UNLIKELY(ctx->locked)) {
      stream_return_(f, false, "already locked");
      return;
    }
    ctx->lock = (upd_file_lock_t) {
      .file  = ctx->bin,
      .udata = f,
      .cb    = stream_lock_bin_cb_,
    };
    if (HEDLEY_UNLIKELY(!upd_file_lock(&ctx->lock))) {
      stream_return_(f, false, "lock refused");
    }
    return;
  }

  if (upd_streq_c("unlock", cmd->ptr, cmd->size)) {
    if (HEDLEY_UNLIKELY(!ctx->locked)) {
      stream_return_(f, false, "already unlocked");
      return;
    }
    stream_unlock_(f);
    stream_return_(f, true, "");
    return;
  }

  if (upd_streq_c("get", cmd->ptr, cmd->size)) {
    if (HEDLEY_UNLIKELY(!ctx->locked)) {
      stream_return_(f, false, "do lock firstly");
      return;
    }
    stream_handle_get_(f);
    return;
  }

  if (upd_streq_c("set", cmd->ptr, cmd->size)) {
    stream_return_(f, false, "not permitted");
    return;
  }

  stream_return_(f, false, "unknown command");
  return;
}

static void stream_handle_get_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  stream_t_* ctx = f->ctx;
  prog_t_*   tar = ctx->target->ctx;

  const msgpack_object* path = NULL;

  if (ctx->param) {
    upd_msgpack_find_fields(ctx->param, (upd_msgpack_field_t[]) {
        { .name = "path", .any = &path, },
        { NULL, },
      });
  }

  yaml_node_t* node = yaml_document_get_root_node(&tar->doc);
  if (path != NULL) {
    if (HEDLEY_UNLIKELY(path->type != MSGPACK_OBJECT_ARRAY)) {
      stream_return_(f, false, "invalid path specification");
      return;
    }
    const msgpack_object_array* p = &path->via.array;
    for (size_t i = 0; node && i < p->size; ++i) {
      node = yaml_find_(&tar->doc, node, &p->ptr[i]);
    }
    if (HEDLEY_UNLIKELY(node == NULL)) {
      stream_return_(f, false, "no such object found");
      return;
    }
  }

  msgpack_packer* pk = &ctx->mpk.pk;

  ctx->broken =
    msgpack_pack_map(pk, 2) ||

      upd_msgpack_pack_cstr(pk, "success") ||
      upd_msgpack_pack_bool(pk, true) ||

      upd_msgpack_pack_cstr(pk, "result");

  stream_pack_yaml_(f, node, 0);
  if (HEDLEY_UNLIKELY(ctx->broken)) {
    upd_iso_msgf(iso, LOG_PREFIX_"msgpack pack failure\n");
  }

  upd_file_trigger(f, UPD_FILE_UPDATE);
  stream_handle_next_(f);
}

static void stream_return_(upd_file_t* f, bool success, const char* msg) {
  upd_iso_t* iso = f->iso;
  stream_t_* ctx = f->ctx;

  msgpack_packer* pk = &ctx->mpk.pk;

  ctx->broken =
    msgpack_pack_map(pk, 2) ||

      upd_msgpack_pack_cstr(pk, "success") ||
      upd_msgpack_pack_bool(pk, success) ||

      upd_msgpack_pack_cstr(pk, "msg") ||
      upd_msgpack_pack_cstr(pk, msg);

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    upd_iso_msgf(iso, LOG_PREFIX_"msgpack pack failure\n");
  }
  upd_file_trigger(f, UPD_FILE_UPDATE);
  stream_handle_next_(f);
}

static void stream_unlock_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;
  prog_t_*   tar = ctx->target->ctx;

  assert(ctx->locked);
  assert(tar->refcnt);

  --tar->refcnt;
  ctx->locked = false;
  upd_file_unlock(&ctx->lock);
}

static void stream_pack_yaml_(upd_file_t* f, yaml_node_t* node, size_t depth) {
  stream_t_*       ctx = f->ctx;
  prog_t_*         tar = ctx->target->ctx;
  yaml_document_t* doc = &tar->doc;
  msgpack_packer*  pk  = &ctx->mpk.pk;

  if (HEDLEY_UNLIKELY(depth >= MAX_DEPTH_)) {
    ctx->broken |= upd_msgpack_pack_cstr(pk, "##TOO DEEP NODE##");
    return;
  }

  if (HEDLEY_UNLIKELY(node == NULL)) {
    ctx->broken |= msgpack_pack_nil(pk);
    return;
  }

  switch (node->type) {
  case YAML_SCALAR_NODE:
    stream_pack_scalar_(f, node);
    return;

  case YAML_SEQUENCE_NODE: {
    yaml_node_item_t* itr = node->data.sequence.items.start;
    yaml_node_item_t* end = node->data.sequence.items.top;

    ctx->broken |= msgpack_pack_array(pk, end-itr);
    for (; itr < end; ++itr) {
      stream_pack_yaml_(f, yaml_document_get_node(doc, *itr), depth+1);
    }
  } return;

  case YAML_MAPPING_NODE: {
    yaml_node_pair_t* itr = node->data.mapping.pairs.start;
    yaml_node_pair_t* end = node->data.mapping.pairs.top;

    ctx->broken |= msgpack_pack_map(pk, end-itr);
    for (; itr < end; ++itr) {
      stream_pack_yaml_(f, yaml_document_get_node(doc, itr->key),   depth+1);
      stream_pack_yaml_(f, yaml_document_get_node(doc, itr->value), depth+1);
    }
  } return;

  default:
    ctx->broken |= msgpack_pack_nil(pk);
    return;
  }
}

static void stream_pack_scalar_(upd_file_t* f, const yaml_node_t* node) {
  stream_t_*      ctx = f->ctx;
  msgpack_packer* pk  = &ctx->mpk.pk;

  const uint8_t* v    = node->data.scalar.value;
  const size_t   vlen = node->data.scalar.length;

  if (HEDLEY_UNLIKELY(node->data.scalar.style != YAML_PLAIN_SCALAR_STYLE)) {
    goto SKIP;
  }
  if (HEDLEY_UNLIKELY(vlen == 0)) {
    ctx->broken |= msgpack_pack_nil(pk);
    return;
  }

  uint8_t str[32];
  if (HEDLEY_UNLIKELY(vlen >= sizeof(str))) {
    goto SKIP;
  }
  utf8ncpy(str, v, vlen);
  str[vlen] = 0;

  char* end;

  const intmax_t i = strtoumax((char*) str, &end, 0);
  if (HEDLEY_UNLIKELY((uint8_t*) end == str+vlen && INTMAX_MIN < i && i < INTMAX_MAX)) {
    ctx->broken |= msgpack_pack_int64(pk, i);
    return;
  }
  const double d = strtod((char*) str, &end);
  if (HEDLEY_UNLIKELY((uint8_t*) end == str+vlen && isfinite(d))) {
    ctx->broken |= msgpack_pack_double(pk, d);
    return;
  }

SKIP:
  ctx->broken |= msgpack_pack_str_with_body(pk, v, vlen);
}


static yaml_node_t* yaml_find_(
    yaml_document_t* doc, yaml_node_t* node, const msgpack_object* term) {
  switch (node->type) {
  case YAML_MAPPING_NODE: {
    if (HEDLEY_UNLIKELY(term->type != MSGPACK_OBJECT_STR)) {
      return NULL;
    }
    const msgpack_object_str* str = &term->via.str;

    const yaml_node_pair_t* itr = node->data.mapping.pairs.start;
    const yaml_node_pair_t* end = node->data.mapping.pairs.top;
    for (; itr < end; ++itr) {
      const yaml_node_t* key = yaml_document_get_node(doc, itr->key);
      if (HEDLEY_UNLIKELY(key == NULL || key->type != YAML_SCALAR_NODE)) {
        continue;
      }
      const bool match = upd_streq(
        key->data.scalar.value, key->data.scalar.length, str->ptr, str->size);
      if (HEDLEY_UNLIKELY(match)) {
        return yaml_document_get_node(doc, itr->value);
      }
    }
  } return NULL;

  case YAML_SEQUENCE_NODE: {
    if (HEDLEY_UNLIKELY(term->type != MSGPACK_OBJECT_POSITIVE_INTEGER)) {
      return NULL;
    }
    const size_t index = term->via.u64;

    const yaml_node_item_t* itr = node->data.sequence.items.start + index;
    const yaml_node_item_t* end = node->data.sequence.items.top;
    if (HEDLEY_LIKELY(itr < end)) {
      return yaml_document_get_node(doc, *itr);
    }
  } return NULL;

  default:
    return NULL;
  }
}


static void prog_watch_bin_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->udata;
  prog_t_*    ctx = f->ctx;

  switch (w->event) {
  case UPD_FILE_UPDATE:
    ctx->clean = false;
    upd_file_trigger(f, UPD_FILE_UPDATE);
    break;
  }
}


static void stream_lock_bin_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  stream_t_*  ctx = f->ctx;
  prog_t_*    tar = ctx->target->ctx;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    stream_return_(f, false, "lock cancelled");
    return;
  }

  ctx->locked = true;
  if (HEDLEY_UNLIKELY(tar->clean || tar->refcnt++)) {
    stream_return_(f, true, "");
    return;
  }

  const bool ok = upd_req_with_dup(&(upd_req_t) {
      .file = ctx->bin,
      .type = UPD_REQ_STREAM_READ,
      .stream = { .io = {
        .size = SIZE_MAX,
      }, },
      .udata = f,
      .cb    = stream_read_bin_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    stream_return_(f, true, "backend read refused");
    return;
  }
}

static void stream_read_bin_cb_(upd_req_t* req) {
  upd_file_t* f   = req->udata;
  upd_iso_t*  iso = f->iso;
  stream_t_*  ctx = f->ctx;
  prog_t_*    tar = ctx->target->ctx;

  const bool     ok   = req->result == UPD_REQ_OK;
  const uint8_t* buf  = req->stream.io.buf;
  const size_t   len  = req->stream.io.size;
  const bool     tail = req->stream.io.tail;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!ok)) {
    stream_return_(f, true, "backend read failure");
    return;
  }
  if (HEDLEY_UNLIKELY(!tail)) {
    stream_return_(f, true, "data is too huge");
    return;
  }

  yaml_parser_t parser;
  if (HEDLEY_UNLIKELY(!yaml_parser_initialize(&parser))) {
    upd_iso_msgf(iso, LOG_PREFIX_"yaml parser allocation failure\n");
    stream_return_(f, true, "parser allocation failure");
    return;
  }

  yaml_document_delete(&tar->doc);
  tar->doc = (yaml_document_t) {0};

  yaml_parser_set_input_string(&parser, buf, len);
  const bool parse = yaml_parser_load(&parser, &tar->doc);
  yaml_parser_delete(&parser);

  if (HEDLEY_UNLIKELY(!parse)) {
    stream_return_(f, true, "yaml parser failure");
    return;
  }
  stream_return_(f, true, "");
}
