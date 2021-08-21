#include "common.h"


typedef struct mix_t_ mix_t_;


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
stream_msgpack_cb_(
  upd_msgpack_t* mpk);

static
void
stream_proto_parse_cb_(
  upd_proto_parse_t* par);

static
void
stream_mix_cb_(
  mix_t_* mix);

const upd_driver_t snd_stream = {
  .name   = (uint8_t*) "upd.sound.dev.stream_",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_DSTREAM,
    0,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


struct mix_t_ {
  upd_file_t* dev;
  upd_file_t* tensor;

  uint64_t time;  /* absolute */

  unsigned locked : 1;

  upd_file_lock_t lock;
  upd_req_t       req;

  const char* err;

  void* udata;
  void
  (*cb)(
    mix_t_* mix);
};

static
bool
mix_with_dup_(
  const mix_t_* src);

static
void
mix_finalize_(
  mix_t_* mix);

static
void
mix_lock_tensor_cb_(
  upd_file_lock_t* k);

static
void
mix_fetch_tensor_cb_(
  upd_req_t* req);

static
void
mix_flush_tensor_cb_(
  upd_req_t* req);


static bool stream_init_(upd_file_t* f) {
  snd_stream_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }

  if (HEDLEY_UNLIKELY(!upd_msgpack_init(&ctx->mpk))) {
    upd_free(&ctx);
    return false;
  }
  ctx->mpk.udata = f;
  ctx->mpk.cb    = stream_msgpack_cb_;

  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  snd_stream_t* ctx = f->ctx;

  upd_msgpack_deinit(&ctx->mpk);

  upd_file_unref(ctx->dev);
  upd_free(&ctx);
}

static bool stream_handle_(upd_req_t* req) {
  upd_file_t*   f   = req->file;
  snd_stream_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->mpk.broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  switch (req->type) {
  case UPD_REQ_DSTREAM_READ:
  case UPD_REQ_DSTREAM_WRITE:
    return upd_msgpack_handle(&ctx->mpk, req);

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static void stream_return_(upd_file_t* f, bool ok, const char* msg) {
  snd_stream_t* ctx = f->ctx;

  msgpack_packer* pk = &ctx->mpk.pk;

  ctx->mpk.broken |=
    msgpack_pack_map(pk, 2) ||

      upd_msgpack_pack_cstr(pk, "success") ||
      upd_msgpack_pack_bool(pk, ok) ||

      upd_msgpack_pack_cstr(pk, "msg") ||
      upd_msgpack_pack_cstr(pk, msg);

  upd_file_trigger(f, UPD_FILE_UPDATE);
  stream_msgpack_cb_(&ctx->mpk);
}

static void stream_msgpack_cb_(upd_msgpack_t* mpk) {
  upd_file_t*   f   = mpk->udata;
  upd_iso_t*    iso = f->iso;
  snd_stream_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!mpk->busy)) {
    msgpack_unpacked_init(&ctx->upkd);
  }

  if (HEDLEY_UNLIKELY(!upd_msgpack_pop(mpk, &ctx->upkd))) {
    msgpack_unpacked_destroy(&ctx->upkd);
    if (HEDLEY_UNLIKELY(mpk->broken)) {
      upd_file_trigger(f, UPD_FILE_UPDATE);
    }
    if (mpk->busy) {
      mpk->busy = false;
      upd_file_unref(f);
    }
    return;
  }

  if (HEDLEY_UNLIKELY(!mpk->busy)) {
    mpk->busy = true;
    upd_file_ref(f);
  }

  ctx->par = (upd_proto_parse_t) {
    .iso   = iso,
    .src   = &ctx->upkd.data,
    .iface = UPD_PROTO_ENCODER,
    .udata = f,
    .cb    = stream_proto_parse_cb_,
  };
  upd_proto_parse(&ctx->par);
}

static void stream_proto_parse_cb_(upd_proto_parse_t* par) {
  upd_file_t*   f   = par->udata;
  snd_stream_t* ctx = f->ctx;
  snd_dev_t*    dev = ctx->dev->ctx;

  const upd_proto_msg_t* msg = &par->msg;
  if (HEDLEY_UNLIKELY(par->err)) {
    stream_return_(f, false, par->err);
    return;
  }

  msgpack_packer* pk = &ctx->mpk.pk;

  switch (msg->cmd) {
  case UPD_PROTO_ENCODER_INFO:
    ctx->mpk.broken |=
      msgpack_pack_map(pk, 2)                                               ||
        upd_msgpack_pack_cstr(pk, "success")                                ||
          msgpack_pack_true(pk)                                             ||
        upd_msgpack_pack_cstr(pk, "result")                                 ||
          msgpack_pack_map(pk, 4)                                           ||
            upd_msgpack_pack_cstr(pk, "description")                        ||
              upd_msgpack_pack_cstr(pk, "audio playback device")            ||
            upd_msgpack_pack_cstr(pk, "type")                               ||
              upd_msgpack_pack_cstr(pk, "audio")                            ||
            upd_msgpack_pack_cstr(pk, "initParam")                          ||
              msgpack_pack_map(pk, 2)                                       ||
                upd_msgpack_pack_cstr(pk, "utimeDen")                       ||
                msgpack_pack_map(pk, 2)                                     ||
                  upd_msgpack_pack_cstr(pk, "type")                         ||
                  upd_msgpack_pack_cstr(pk, "integer")                      ||
                  upd_msgpack_pack_cstr(pk, "description")                  ||
                  upd_msgpack_pack_cstr(pk, "denominator part of unittime") ||
                upd_msgpack_pack_cstr(pk, "utimeNum")                       ||
                msgpack_pack_map(pk, 2)                                     ||
                  upd_msgpack_pack_cstr(pk, "type")                         ||
                  upd_msgpack_pack_cstr(pk, "integer")                      ||
                  upd_msgpack_pack_cstr(pk, "description")                  ||
                  upd_msgpack_pack_cstr(pk, "numerator part of unittime")   ||
            upd_msgpack_pack_cstr(pk, "frameParam")                         ||
              msgpack_pack_map(pk, 1)                                       ||
                upd_msgpack_pack_cstr(pk, "time")                           ||
                msgpack_pack_map(pk, 2)                                     ||
                  upd_msgpack_pack_cstr(pk, "type")                         ||
                  upd_msgpack_pack_cstr(pk, "integer")                      ||
                  upd_msgpack_pack_cstr(pk, "description")                  ||
                  upd_msgpack_pack_cstr(pk, "");
    upd_file_trigger(f, UPD_FILE_UPDATE);
    stream_msgpack_cb_(&ctx->mpk);
    return;

  case UPD_PROTO_ENCODER_INIT: {
    if (HEDLEY_UNLIKELY(ctx->init)) {
      stream_return_(f, false, "already initialized");
      return;
    }
    uintmax_t utime_den = 0;
    uintmax_t utime_num = 0;

    const char* invalid =
      upd_msgpack_find_fields(msg->param, (upd_msgpack_field_t[]) {
          { .name = "utimeDen", .ui = &utime_den, .required = true, },
          { .name = "utimeNum", .ui = &utime_num, .required = true, },
          { NULL, },
        });
    if (HEDLEY_UNLIKELY(utime_den == 0 || utime_num == 0 || invalid)) {
      stream_return_(f, false, "invalid param");
      return;
    }
    ctx->utime_den = utime_den;
    ctx->utime_num = utime_num * dev->srate * dev->ch;
    ctx->tbase     = atomic_load(&dev->now);
    stream_return_(f, true, "");
  } return;

  case UPD_PROTO_ENCODER_FRAME: {
    if (HEDLEY_UNLIKELY(!ctx->init)) {
      stream_return_(f, false, "not initialized");
      return;
    }

    uintmax_t time = 0;

    const char* invalid =
      upd_msgpack_find_fields(msg->param, (upd_msgpack_field_t[]) {
          { .name = "time", .ui = &time, .required = true, },
          { NULL, },
        });
    if (HEDLEY_UNLIKELY(invalid)) {
      stream_return_(f, false, "invalid param");
      return;
    }

    time = time * ctx->utime_num / ctx->utime_den + ctx->tbase;

    const bool ok = mix_with_dup_(&(mix_t_) {
        .dev    = ctx->dev,
        .tensor = msg->encoder_frame.file,
        .time   = time,
        .udata  = f,
        .cb     = stream_mix_cb_,
      });
    if (HEDLEY_UNLIKELY(!ok)) {
      stream_return_(f, false, "task allocation failure");
      return;
    }
  } return;

  case UPD_PROTO_ENCODER_FINALIZE:
    if (HEDLEY_UNLIKELY(!ctx->init)) {
      stream_return_(f, false, "not initialized");
      return;
    }
    ctx->init = false;
    return;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }
}

static void stream_mix_cb_(mix_t_* mix) {
  upd_file_t* f   = mix->udata;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_LIKELY(mix->err == NULL)) {
    stream_return_(f, true, "");
  } else {
    stream_return_(f, false, mix->err);
  }
  upd_iso_unstack(iso, mix);
}


static bool mix_with_dup_(const mix_t_* src) {
  upd_file_t* dev = src->dev;
  upd_iso_t*  iso = dev->iso;

  mix_t_* mix = upd_iso_stack(iso, sizeof(*mix));
  if (HEDLEY_UNLIKELY(mix == NULL)) {
    return false;
  }
  *mix = *src;

  mix->lock = (upd_file_lock_t) {
    .file  = src->tensor,
    .ex    = true,
    .udata = mix,
    .cb    = mix_lock_tensor_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&mix->lock))) {
    upd_iso_unstack(iso, mix);
    return false;
  }
  return true;
}

static void mix_finalize_(mix_t_* mix) {
  if (HEDLEY_UNLIKELY(mix->locked)) {
    upd_file_unlock(&mix->lock);
  }
  mix->cb(mix);
}

static void mix_lock_tensor_cb_(upd_file_lock_t* k) {
  mix_t_* mix = k->udata;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    mix->err = "tensor lock cancelled";
    goto ABORT;
  }
  mix->locked = true;

  mix->req = (upd_req_t) {
    .file  = mix->tensor,
    .type  = UPD_REQ_TENSOR_FETCH,
    .udata = mix,
    .cb    = mix_fetch_tensor_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&mix->req))) {
    mix->err = "tensor fetch refusal";
    goto ABORT;
  }
  return;

ABORT:
  mix_finalize_(mix);
}

static void mix_fetch_tensor_cb_(upd_req_t* req) {
  mix_t_* mix = req->udata;

  if (HEDLEY_UNLIKELY(!req->result != UPD_REQ_OK)) {
    mix->err = "tensor fetch failure";
    goto ABORT;
  }

  const upd_req_tensor_data_t* data = &req->tensor.data;

  if (HEDLEY_UNLIKELY(!snd_mix(mix->dev, data, mix->time))) {
    mix->err = "failed to write ring buffer";
    /* goto ABORT; Don't abort! It must be flushed. */
  }

  mix->req = (upd_req_t) {
    .file  = mix->tensor,
    .type  = UPD_REQ_TENSOR_FLUSH,
    .udata = mix,
    .cb    = mix_flush_tensor_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&mix->req))) {
    goto ABORT;
  }
  return;

ABORT:
  mix_finalize_(mix);
}

static void mix_flush_tensor_cb_(upd_req_t* req) {
  mix_t_* mix = req->udata;
  mix_finalize_(mix);
}
