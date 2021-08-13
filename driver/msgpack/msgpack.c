#include <stdint.h>

#include <hedley.h>
#include <utf8.h>
#include <msgpack.h>

#define UPD_EXTERNAL_DRIVER
#include <libupd.h>
#undef UPD_EXTERNAL_DRIVER

#include <libupd/array.h>
#include <libupd/memory.h>
#include <libupd/msgpack.h>
#include <libupd/str.h>


#define LOG_PREFIX_ "upd.msgpack: "


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

  msgpack_packer  pk;
  msgpack_sbuffer pkbuf;

  msgpack_unpacker upk;
  msgpack_object   obj;

  size_t refcnt;  /* cache refcnt */
  upd_array_of(msgpack_unpacked*) upkd;

  unsigned clean : 1;
} prog_t_;

typedef struct stream_t_ {
  upd_file_t* file;

  upd_file_t* target;

  upd_file_t*     bin;
  upd_file_lock_t lock;
  size_t          bin_offset;

  upd_msgpack_t mpk;

  msgpack_unpacked*         upkd;  /* data is on iso stack */
  const msgpack_object_map* param;

  unsigned busy   : 1;
  unsigned locked : 1;
  unsigned broken : 1;
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

static
void
prog_drop_cache_(
  upd_file_t* f);

static const upd_driver_t prog_driver_ = {
  .name = (uint8_t*) "upd.msgpack",
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
stream_handle_obj_(
  upd_file_t* f);

static
void
stream_handle_get_(
  upd_file_t* f);

static
void
stream_handle_set_(
  upd_file_t* f);

static
void
stream_teardown_(
  stream_t_* ctx);

static
void
stream_unlock_(
  stream_t_* ctx);

static
void
stream_return_(
  upd_file_t* f,
  bool        success,
  const char* msg);

static const upd_driver_t stream_driver_ = {
  .name = (uint8_t*) "upd.msgpack.stream_",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_STREAM,
    0,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


static
msgpack_object*
mpk_find_(
  msgpack_object*       obj,
  const msgpack_object* term);

static
msgpack_object*
mpk_set_nil_(
  msgpack_object*       obj,
  msgpack_zone*         zone,
  const msgpack_object* key);


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

static
void
stream_truncate_bin_cb_(
  upd_req_t* req);

static
void
stream_write_bin_cb_(
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
    upd_iso_msgf(iso, "context allocation failure\n");
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

  if (HEDLEY_UNLIKELY(!msgpack_unpacker_init(&ctx->upk, 1024))) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    upd_iso_msgf(iso, LOG_PREFIX_"backend watch refused\n");
    return false;
  }

  msgpack_packer_init(&ctx->pk, &ctx->pkbuf, msgpack_sbuffer_write);

  f->ctx      = ctx;
  f->mimetype = (uint8_t*) "upd/prog;encoding=msgpack;interfaces=object";
  return true;
}

static void prog_deinit_(upd_file_t* f) {
  prog_t_* ctx = f->ctx;

  upd_file_unwatch(&ctx->watch);
  prog_drop_cache_(f);

  msgpack_sbuffer_destroy(&ctx->pkbuf);
  msgpack_unpacker_destroy(&ctx->upk);

  upd_free(&ctx);
}

static bool prog_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;

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
}

static void prog_drop_cache_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  prog_t_*   ctx = f->ctx;

  assert(ctx->refcnt == 0);

  for (size_t i = 0; i < ctx->upkd.n; ++i) {
    msgpack_unpacked* upkd = ctx->upkd.p[i];
    msgpack_unpacked_destroy(upkd);
    upd_iso_unstack(iso, upkd);
  }
  upd_array_clear(&ctx->upkd);
}


static bool stream_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  stream_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    upd_iso_msgf(iso, LOG_PREFIX_"stream context allocation failure\n");
    return false;
  }
  *ctx = (stream_t_) {
    .file = f,
    .mpk  = {
      .iso = iso,
    },
  };

  if (HEDLEY_UNLIKELY(!upd_msgpack_init(&ctx->mpk))) {
    upd_free(&ctx);
    return false;
  }
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;

  upd_msgpack_deinit(&ctx->mpk);

  ctx->file = NULL;
  if (HEDLEY_UNLIKELY(ctx->locked)) {
    stream_unlock_(ctx);
  } else {
    stream_teardown_(ctx);
  }
}

static bool stream_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  switch (req->type) {
  case UPD_REQ_DSTREAM_WRITE: {
    if (HEDLEY_UNLIKELY(!upd_msgpack_unpack(&ctx->mpk, &req->stream.io))) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    if (HEDLEY_UNLIKELY(!ctx->busy)) {
      stream_handle_next_(f);
    }
    req->result = UPD_REQ_OK;
    req->cb(req);
  } return true;

  case UPD_REQ_DSTREAM_READ: {
    req->stream.io = (upd_req_stream_io_t) {
      .buf  = (uint8_t*) ctx->mpk.out.data,
      .size = ctx->mpk.out.size,
    };
    uint8_t* ptr = (void*) msgpack_sbuffer_release(&ctx->mpk.out);
    req->result = UPD_REQ_OK;
    req->cb(req);
    free(ptr);
  } return true;

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
  stream_handle_obj_(f);
}

static void stream_handle_obj_(upd_file_t* f) {
  stream_t_*        ctx  = f->ctx;
  msgpack_unpacked* upkd = ctx->upkd;

  if (HEDLEY_UNLIKELY(upkd->data.type != MSGPACK_OBJECT_MAP)) {
    stream_return_(f, false, "root object must be a map");
    return;
  }

  /* Don't forget to destroy and unstack upkd. */

  const msgpack_object_str* iface = NULL;
  const msgpack_object_str* cmd   = NULL;

  ctx->param = NULL;
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

  const bool lock   = upd_streq_c("lock", cmd->ptr, cmd->size);
  const bool exlock = upd_streq_c("exlock", cmd->ptr, cmd->size);
  if (lock || exlock) {
    if (HEDLEY_UNLIKELY(ctx->locked)) {
      stream_return_(f, false, "already locked");
      return;
    }
    ctx->lock = (upd_file_lock_t) {
      .file  = ctx->bin,
      .ex    = exlock,
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
    stream_unlock_(ctx);
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
    if (HEDLEY_UNLIKELY(!ctx->locked || !ctx->lock.ex)) {
      stream_return_(f, false, "do exlock firstly");
      return;
    }
    stream_handle_set_(f);
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

  msgpack_object* obj = &tar->obj;
  if (path != NULL) {
    if (HEDLEY_UNLIKELY(path->type != MSGPACK_OBJECT_ARRAY)) {
      stream_return_(f, false, "invalid path specification");
      return;
    }
    const msgpack_object_array* p = &path->via.array;

    for (size_t i = 0; obj && i < p->size; ++i) {
      obj = mpk_find_(obj, &p->ptr[i]);
    }
    if (HEDLEY_UNLIKELY(obj == NULL)) {
      stream_return_(f, false, "no such object found");
      return;
    }
  }

  msgpack_packer* pk = &ctx->mpk.pk;
  ctx->broken =
    msgpack_pack_map(pk, 2) ||

      upd_msgpack_pack_cstr(pk, "success") ||
      msgpack_pack_true(pk) ||

      upd_msgpack_pack_cstr(pk, "result") ||
      msgpack_pack_object(pk, *obj);

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    upd_iso_msgf(iso,
      LOG_PREFIX_"msgpack pack failure (maybe requested object is too huge)\n");
  }
  upd_file_trigger(f, UPD_FILE_UPDATE);
  stream_handle_next_(f);
}

static void stream_handle_set_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;
  prog_t_*   tar = ctx->target->ctx;

  const msgpack_object* path  = NULL;
  const msgpack_object* value = NULL;

  if (ctx->param) {
    upd_msgpack_find_fields(ctx->param, (upd_msgpack_field_t[]) {
        { .name = "path",  .any = &path,  },
        { .name = "value", .any = &value, },
        { NULL, },
      });
  }

  msgpack_unpacked* upkd = ctx->upkd;
  ctx->upkd = NULL;
  if (HEDLEY_UNLIKELY(!upd_array_insert(&tar->upkd, upkd, SIZE_MAX))) {
    stream_return_(f, false, "zone insertion failure");
    return;
  }

  msgpack_object* obj = &tar->obj;

  if (path != NULL) {
    if (HEDLEY_UNLIKELY(path->type != MSGPACK_OBJECT_ARRAY)) {
      stream_return_(f, false, "invalid path specification");
      return;
    }
    const msgpack_object_array* p = &path->via.array;
    for (size_t i = 0; i < p->size; ++i) {
      msgpack_object* par = obj;
      obj = mpk_find_(obj, &p->ptr[i]);
      if (obj == NULL) {
        obj = mpk_set_nil_(par, upkd->zone, &p->ptr[i]);
        if (HEDLEY_UNLIKELY(obj == NULL)) {
          stream_return_(f, false, "new object insertion failure");
          return;
        }
        tar->clean = false;
      }
    }
  }

  tar->clean = false;
  if (HEDLEY_LIKELY(value)) {
    *obj = *value;
  } else {
    obj->type = MSGPACK_OBJECT_NIL;
  }
  stream_return_(f, true, "");
  return;
}

static void stream_teardown_(stream_t_* ctx) {
  upd_file_unref(ctx->bin);
  upd_file_unref(ctx->target);
  upd_free(&ctx);
}

static void stream_unlock_(stream_t_* ctx) {
  upd_iso_t* iso = ctx->target->iso;
  prog_t_*   tar = ctx->target->ctx;

  assert(ctx->locked);
  assert(tar->refcnt);

  const char* msg = "";

  ctx->locked = false;
  if (HEDLEY_UNLIKELY(--tar->refcnt)) {
    goto FINALIZE;
  }

  if (HEDLEY_UNLIKELY(!tar->clean)) {
    msgpack_pack_object(&tar->pk, tar->obj);

    const bool truncate = upd_req_with_dup(&(upd_req_t) {
        .file = ctx->bin,
        .type = UPD_REQ_STREAM_TRUNCATE,
        .stream = { .io = {
          .size = tar->pkbuf.size,
        }, },
        .udata = ctx,
        .cb    = stream_truncate_bin_cb_,
      });
    if (HEDLEY_LIKELY(truncate)) {
      return;
    }
    msg = "backend truncate refused, your changes may be lost";
  }
  tar->clean = false;
  prog_drop_cache_(ctx->target);

FINALIZE:
  upd_file_unlock(&ctx->lock);

  if (HEDLEY_LIKELY(ctx->file)) {
    stream_return_(ctx->file, true, msg);
  } else {
    stream_teardown_(ctx);
    if (msg[0]) {
      upd_iso_msgf(iso, LOG_PREFIX_"%s\n", msg);
    }
  }
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


static msgpack_object* mpk_find_(
    msgpack_object* obj, const msgpack_object* term) {
  switch (obj->type) {
  case MSGPACK_OBJECT_MAP:
    return (void*) upd_msgpack_find_obj(&obj->via.map, term);

  case MSGPACK_OBJECT_ARRAY:
    if (HEDLEY_UNLIKELY(term->type != MSGPACK_OBJECT_POSITIVE_INTEGER)) {
      return NULL;
    }
    if (HEDLEY_UNLIKELY(term->via.u64 >= obj->via.array.size)) {
      return NULL;
    }
    return &obj->via.array.ptr[term->via.u64];

  default:
    return NULL;
  }
}

static msgpack_object* mpk_set_nil_(
    msgpack_object* obj, msgpack_zone* zone, const msgpack_object* key) {
  switch (obj->type) {
  case MSGPACK_OBJECT_MAP: {
    msgpack_object_map* m = &obj->via.map;

    msgpack_object_kv* ptr =
      msgpack_zone_malloc(zone, sizeof(*ptr)*(m->size+1));
    if (HEDLEY_UNLIKELY(ptr == NULL)) {
      return NULL;
    }
    memcpy(ptr, m->ptr, sizeof(*ptr)*m->size);

    msgpack_object_kv* ret = &ptr[m->size];
    *ret = (msgpack_object_kv) {
      .key = *key,
      .val = {
        .type = MSGPACK_OBJECT_NIL,
      },
    };
    ++m->size;
    m->ptr = ptr;
    return &ret->val;
  }

  case MSGPACK_OBJECT_ARRAY: {
    /* TODO */
  } return NULL;

  default: {
    *obj = (msgpack_object) {
      .type = MSGPACK_OBJECT_MAP,
    };
  } return mpk_set_nil_(obj, zone, key);
  }
}


static void prog_watch_bin_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->udata;
  upd_iso_t*  iso = f->iso;
  prog_t_*    ctx = f->ctx;

  if (w->event == UPD_FILE_UPDATE) {
    if (HEDLEY_UNLIKELY(ctx->refcnt)) {
      upd_iso_msgf(iso,
        LOG_PREFIX_"src file is modified by outsider, "
        "but this changes may be lost (%s)\n", f->npath);
    } else {
      ctx->clean = false;
      upd_file_trigger(f, UPD_FILE_UPDATE);
    }
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

  tar->obj.type = MSGPACK_OBJECT_NIL;

  ctx->bin_offset = 0;
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

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    upd_iso_unstack(iso, req);
    stream_return_(f, true, "backend read failure");
    return;
  }

  const uint8_t* buf  = req->stream.io.buf;
  const size_t   len  = req->stream.io.size;
  const bool     tail = req->stream.io.tail;
  ctx->bin_offset += len;

  if (msgpack_unpacker_buffer_capacity(&tar->upk) < len) {
    if (HEDLEY_UNLIKELY(!msgpack_unpacker_reserve_buffer(&tar->upk, len))) {
      upd_iso_unstack(iso, req);
      stream_return_(f, true, "unpacker buffer allocation failure");
      return;
    }
  }
  memcpy(msgpack_unpacker_buffer(&tar->upk), buf, len);
  msgpack_unpacker_buffer_consumed(&tar->upk, len);

  msgpack_unpacked* upkd = upd_iso_stack(iso, sizeof(*upkd));;
  if (HEDLEY_UNLIKELY(upkd == NULL)) {
    upd_iso_unstack(iso, req);
    stream_return_(f, true, "object allocation failure");
    return;
  }
  msgpack_unpacked_init(upkd);

  const msgpack_unpack_return ret = msgpack_unpacker_next(&tar->upk, upkd);
  switch (ret) {
  case MSGPACK_UNPACK_SUCCESS:
    upd_iso_unstack(iso, req);
    if (HEDLEY_UNLIKELY(!upd_array_insert(&tar->upkd, upkd, SIZE_MAX))) {
      msgpack_unpacked_destroy(upkd);
      upd_iso_unstack(iso, upkd);
      stream_return_(f, true, "object insertion failure");
      return;
    }
    tar->obj = upkd->data;
    stream_return_(f, true, "");
    return;

  case MSGPACK_UNPACK_CONTINUE:
    msgpack_unpacked_destroy(upkd);
    upd_iso_unstack(iso, upkd);

    if (HEDLEY_UNLIKELY(len == 0 || tail)) {
      upd_iso_unstack(iso, req);
      stream_return_(f, true, "loaded data is incompleted");
      return;
    }
    *req = (upd_req_t) {
      .file = ctx->bin,
      .type = UPD_REQ_STREAM_READ,
      .stream = { .io = {
        .offset = ctx->bin_offset,
        .size   = SIZE_MAX,
      }, },
      .udata = f,
      .cb    = stream_read_bin_cb_,
    };
    if (HEDLEY_UNLIKELY(!upd_req(req))) {
      upd_iso_unstack(iso, req);
      stream_return_(f, true, "backend continuous read refused");
      return;
    }
    return;

  default:
    msgpack_unpacked_destroy(upkd);
    upd_iso_unstack(iso, upkd);

    upd_iso_unstack(iso, req);
    stream_return_(f, true, "invalid msgpack format");
    return;
  }
}

static void stream_truncate_bin_cb_(upd_req_t* req) {
  stream_t_* ctx = req->udata;
  upd_iso_t* iso = ctx->target->iso;
  prog_t_*   tar = ctx->target->ctx;

  const char* msg = "";

  const bool ok = req->result == UPD_REQ_OK;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!ok)) {
    msg = "backend truncate failure, your changes may be lost";
    goto ABORT;
  }

  const bool write = upd_req_with_dup(&(upd_req_t) {
      .file = ctx->bin,
      .type = UPD_REQ_STREAM_WRITE,
      .stream = { .io = {
        .size = tar->pkbuf.size,
        .buf  = (uint8_t*) tar->pkbuf.data,
      }, },
      .udata = ctx,
      .cb    = stream_write_bin_cb_,
    });
  if (HEDLEY_UNLIKELY(!write)) {
    msg = "backend write refused, your changes may be lost";
  }
  return;

ABORT:
  prog_drop_cache_(ctx->target);
  upd_file_unlock(&ctx->lock);

  if (HEDLEY_LIKELY(ctx->file)) {
    stream_return_(ctx->file, true, msg);
  } else {
    stream_teardown_(ctx);
    if (msg[0]) {
      upd_iso_msgf(iso, LOG_PREFIX_"%s\n", msg);
    }
  }
}

static void stream_write_bin_cb_(upd_req_t* req) {
  stream_t_* ctx = req->udata;
  upd_iso_t* iso = ctx->target->iso;
  prog_t_*   tar = ctx->target->ctx;

  const char* msg = "";

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    msg = "backend write failure, your changes may be lost";
  }
  upd_iso_unstack(iso, req);
  msgpack_sbuffer_clear(&tar->pkbuf);

  prog_drop_cache_(ctx->target);
  upd_file_unlock(&ctx->lock);

  if (HEDLEY_LIKELY(ctx->file)) {
    stream_return_(ctx->file, true, msg);
  } else {
    stream_teardown_(ctx);
    if (msg[0]) {
      upd_iso_msgf(iso, LOG_PREFIX_"%s\n", msg);
    }
  }
}
