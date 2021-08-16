#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <hedley.h>
#include <utf8.h>
#include <yaml.h>
#include <zlib-ng.h>

#define UPD_EXTERNAL_DRIVER
#include <libupd.h>
#undef UPD_EXTERNAL_DRIVER

#include <libupd/array.h>
#include <libupd/buf.h>
#include <libupd/msgpack.h>
#include <libupd/proto.h>
#include <libupd/str.h>
#include <libupd/tensor.h>
#include <libupd/yaml.h>

#include "png.h"
#include "reader.h"
#include "writer.h"


#define WRITER_BUFFER_LEN_ (1024*64)  /* = 64 KiB */


static const upd_driver_t png_driver_;

upd_external_t upd = {
  .ver = UPD_VER,
  .drivers = (const upd_driver_t*[]) {
    &png_driver_,
    NULL,
  },
};


typedef struct png_t_    png_t_;
typedef struct stream_t_ stream_t_;
typedef struct io_t_     io_t_;

struct png_t_ {
  upd_file_watch_t watch_self;
  upd_file_watch_t watch_bin;

  uint8_t*  data;
  uint8_t   depth;  /* in bytes */
  uint8_t   spp;
  uint32_t  w, h;

  upd_array_of(upd_req_t*) pending;

  unsigned clean  : 1;
  unsigned broken : 1;
};

struct stream_t_ {
  upd_file_t* target;

  upd_msgpack_t     mpk;
  msgpack_unpacked  upkd;
  upd_proto_parse_t par;
};


struct io_t_ {
  upd_file_t*     bin;
  upd_file_lock_t lock;
  size_t          offset;

  upd_req_t req;

  union {
    png_reader_t reader;
    png_writer_t writer;
  };

  /* for writer */
  uint8_t* buf;
  size_t   buflen;

  void* udata;
  void
  (*cb)(
    io_t_* io);

  const char* msg;

  unsigned locked : 1;
  unsigned ok     : 1;
};


static
bool
png_init_(
  upd_file_t* f);

static
void
png_deinit_(
  upd_file_t* f);

static
bool
png_handle_(
  upd_req_t* req);

static
void
png_handle_pending_(
  upd_file_t* f);

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
png_logf_(
  upd_file_t* f,
  const char* fmt,
  ...);

static const upd_driver_t png_driver_ = {
  .name = (uint8_t*) "upd.png",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_PROG,
    UPD_REQ_TENSOR,
    0,
  },
  .init   = png_init_,
  .deinit = png_deinit_,
  .handle = png_handle_,
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
stream_success_(
  upd_file_t* f);

static
void
stream_error_(
  upd_file_t* f,
  const char* msg);

static const upd_driver_t stream_driver_ = {
  .name = (uint8_t*) "upd.png.stream_",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_DSTREAM,
    0,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


static
bool
reader_start_with_dup_(
  const io_t_* src);

static
void
reader_finalize_(
  io_t_* io);


static
bool
writer_start_with_dup_(
  const io_t_* src);

static
void
writer_finalize_(
  io_t_* io);


static
void
png_watch_self_cb_(
  upd_file_watch_t* w);

static
void
png_watch_bin_cb_(
  upd_file_watch_t* w);

static
void
png_lock_self_for_uncache_cb_(
  upd_file_lock_t* k);

static
void
png_read_cb_(
  io_t_* io);


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
stream_lock_tensor_cb_(
  upd_file_lock_t* k);

static
void
stream_tensor_data_cb_(
  upd_req_t* req);

static
void
stream_lock_png_cb_(
  upd_file_lock_t* k);

static
void
stream_flush_tensor_cb_(
  upd_req_t* req);

static
void
stream_write_cb_(
  io_t_* io);


static
void
reader_lock_bin_cb_(
  upd_file_lock_t* k);

static
void
reader_read_bin_cb_(
  upd_req_t* req);


static
void
writer_lock_bin_cb_(
  upd_file_lock_t* k);

static
void
writer_write_bin_cb_(
  upd_req_t* req);

static
void
writer_truncate_bin_cb_(
  upd_req_t* req);


static bool png_init_(upd_file_t* f) {
  if (HEDLEY_UNLIKELY(!f->npathlen)) {
    png_logf_(f, "empty npath");
    return false;
  }
  if (HEDLEY_UNLIKELY(!f->backend)) {
    png_logf_(f, "requires backend file");
    return false;
  }

  png_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    png_logf_(f, "context allocation failure");
    return false;
  }
  *ctx = (png_t_) {
    .watch_self = {
      .file  = f,
      .udata = f,
      .cb    = png_watch_self_cb_,
    },
    .watch_bin = {
      .file  = f->backend,
      .udata = f,
      .cb    = png_watch_bin_cb_,
    },
  };

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch_self))) {
    png_logf_(f, "self watch failure");
    return false;
  }
  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch_bin))) {
    png_logf_(f, "backend watch failure");
    return false;
  }

  f->ctx      = ctx;
  f->mimetype = (uint8_t*) "image/png";
  //f->mimetype = (uint8_t*) "upd/prog; encoding=msgpack; interfaces=encoder";
  return true;
}

static void png_deinit_(upd_file_t* f) {
  png_t_* ctx = f->ctx;

  upd_file_unwatch(&ctx->watch_self);
  upd_file_unwatch(&ctx->watch_bin);

  upd_free(&ctx->data);
  upd_free(&ctx);
}

static bool png_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;
  png_t_*     ctx = f->ctx;

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

    upd_file_ref(f);
    stctx->target = f;

    req->prog.exec = stf;
    req->result    = UPD_REQ_OK;
    req->cb(req);

    upd_file_unref(stf);
  } return true;

  case UPD_REQ_TENSOR_META:
  case UPD_REQ_TENSOR_DATA:
    /* this req can be caused with inclusive lock,
     * so don't forget parallelism. */
    if (HEDLEY_UNLIKELY(!ctx->clean)) {
      if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->pending, req, SIZE_MAX))) {
        req->result = UPD_REQ_NOMEM;
        return false;
      }
      if (HEDLEY_UNLIKELY(ctx->pending.n > 1)) {
        return true;
      }
      const bool ok = reader_start_with_dup_(&(io_t_) {
          .bin    = f->backend,
          .reader = {
            .pix = { .ptr = ctx->data, },
          },
          .udata  = f,
          .cb     = png_read_cb_,
        });
      if (HEDLEY_UNLIKELY(!ok)) {
        upd_array_clear(&ctx->pending);
        req->result = UPD_REQ_ABORTED;
        return false;
      }
      f->cache = 0;
      return true;
    }

    if (HEDLEY_UNLIKELY(ctx->broken)) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }

    uint32_t reso[3] = {ctx->spp, ctx->w, ctx->h};
    req->tensor.data = (upd_req_tensor_data_t) {
      .meta = {
        .rank = 3,
        .type = ctx->depth == 1? UPD_TENSOR_U8: UPD_TENSOR_U16,
        .reso = reso,
      },
      .ptr  = ctx->data,
      .size = ctx->depth*ctx->spp*ctx->w*ctx->h,
    };
    req->cb(req);
    return true;

  case UPD_REQ_TENSOR_FLUSH:
    req->cb(req);
    return true;

  default:
    req->file = f->backend;
    return upd_req(req);
  }
}

static void png_handle_pending_(upd_file_t* f) {
  png_t_* png = f->ctx;

  for (size_t i = 0; i < png->pending.n; ++i) {
    upd_req_t* req = png->pending.p[i];
    if (HEDLEY_UNLIKELY(!upd_req(req))) {
      req->cb(req);
    }
  }
  upd_array_clear(&png->pending);
}

static void png_logf_(upd_file_t* f, const char* fmt, ...) {
  char temp[256];

  va_list args;
  va_start(args, fmt);
  vsnprintf(temp, sizeof(temp), fmt, args);
  va_end(args);

  upd_iso_msgf(f->iso, "upd.png: %s (%s)\n", temp, f->npath);
}


static bool stream_init_(upd_file_t* f) {
  stream_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (stream_t_) {0};

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
  stream_t_* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->target)) {
    upd_file_unref(ctx->target);
  }

  upd_msgpack_deinit(&ctx->mpk);
  upd_free(&ctx);
}

static bool stream_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->mpk.broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  switch (req->type) {
  case UPD_REQ_DSTREAM_WRITE:
  case UPD_REQ_DSTREAM_READ:
    return upd_msgpack_handle(&ctx->mpk, req);

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static void stream_success_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;

  msgpack_packer* pk = &ctx->mpk.pk;

  ctx->mpk.broken |=
    msgpack_pack_map(pk, 1)                ||
      upd_msgpack_pack_cstr(pk, "success") ||
        msgpack_pack_true(pk);

  upd_file_trigger(f, UPD_FILE_UPDATE);
  stream_msgpack_cb_(&ctx->mpk);
}

static void stream_error_(upd_file_t* f, const char* msg) {
  stream_t_* ctx = f->ctx;

  msgpack_packer* pk = &ctx->mpk.pk;

  ctx->mpk.broken |=
    msgpack_pack_map(pk, 2)                ||
      upd_msgpack_pack_cstr(pk, "success") ||
        msgpack_pack_false(pk)             ||
      upd_msgpack_pack_cstr(pk, "msg")     ||
        upd_msgpack_pack_cstr(pk, msg);

  upd_file_trigger(f, UPD_FILE_UPDATE);
  stream_msgpack_cb_(&ctx->mpk);
}


static bool reader_start_with_dup_(const io_t_* src) {
  upd_iso_t* iso = src->bin->iso;

  io_t_* io = upd_iso_stack(iso, sizeof(*io));
  if (HEDLEY_UNLIKELY(io == NULL)) {
    return false;
  }
  *io = *src;

  if (HEDLEY_UNLIKELY(!png_reader_init(&io->reader))) {
    upd_iso_unstack(iso, io);
    return false;
  }

  io->lock = (upd_file_lock_t) {
    .file  = io->bin,
    .udata = io,
    .cb    = reader_lock_bin_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&io->lock))) {
    upd_iso_unstack(iso, io);
    return false;
  }
  return true;
}

static void reader_finalize_(io_t_* io) {
  if (HEDLEY_LIKELY(io->locked)) {
    upd_file_unlock(&io->lock);
  }
  png_reader_deinit(&io->reader);
  io->cb(io);
}


static bool writer_start_with_dup_(const io_t_* src) {
  upd_iso_t* iso = src->bin->iso;

  io_t_* io = upd_iso_stack(iso, sizeof(*io));
  if (HEDLEY_UNLIKELY(io == NULL)) {
    return false;
  }
  *io = *src;

  io->buflen = WRITER_BUFFER_LEN_;
  if (HEDLEY_UNLIKELY(!upd_malloc(&io->buf, io->buflen))) {
    upd_iso_unstack(iso, io);
    return false;
  }

  if (HEDLEY_UNLIKELY(!png_writer_init(&io->writer))) {
    upd_free(&io->buf);
    upd_iso_unstack(iso, io);
    return false;
  }

  io->lock = (upd_file_lock_t) {
    .file  = io->bin,
    .ex    = true,
    .udata = io,
    .cb    = writer_lock_bin_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&io->lock))) {
    png_writer_deinit(&io->writer);
    upd_free(&io->buf);
    upd_iso_unstack(iso, io);
    return false;
  }
  return true;
}

static void writer_finalize_(io_t_* io) {
  if (HEDLEY_LIKELY(io->locked)) {
    upd_file_unlock(&io->lock);
  }

  upd_free(&io->buf);
  png_writer_deinit(&io->writer);

  io->cb(io);
}


static void png_watch_self_cb_(upd_file_watch_t* w) {
  upd_file_t* f = w->udata;

  if (HEDLEY_LIKELY(w->event == UPD_FILE_UNCACHE)) {
    const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file    = f,
        .ex      = true,
        .timeout = 1,
        .udata   = f,
        .cb      = png_lock_self_for_uncache_cb_,
      });
    if (HEDLEY_UNLIKELY(!lock)) {
      /* ignore error */
    }
  }
}

static void png_watch_bin_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->udata;
  png_t_*     ctx = f->ctx;

  if (HEDLEY_LIKELY(w->event == UPD_FILE_UPDATE)) {
    ctx->clean = false;
    upd_file_trigger(f, UPD_FILE_UPDATE);
  }
}

static void png_lock_self_for_uncache_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  upd_iso_t*  iso = f->iso;
  png_t_*     ctx = f->ctx;

  if (HEDLEY_LIKELY(k->ok)) {
    ctx->clean = false;
    f->cache   = 0;
    upd_free(&ctx->data);
  }
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void png_read_cb_(io_t_* io) {
  upd_file_t* f   = io->udata;
  upd_iso_t*  iso = f->iso;
  png_t_*     ctx = f->ctx;

  png_reader_t* re = &io->reader;
  if (HEDLEY_LIKELY(io->ok)) {
    ctx->broken = false;
    ctx->data   = re->pix.ptr;
    ctx->depth  = re->pix.depth;
    ctx->spp    = re->line.spp;
    ctx->w      = re->ihdr.width;
    ctx->h      = re->ihdr.height;
    f->cache    = re->pix.size;
  } else {
    ctx->broken = true;
    ctx->data   = NULL;
    f->cache    = 0;

    upd_free(&re->pix.ptr);
    png_logf_(f, "png read failure: %s", io->msg);
  }
  upd_iso_unstack(iso, io);

  ctx->clean = true;
  png_handle_pending_(f);
}


static void stream_msgpack_cb_(upd_msgpack_t* mpk) {
  upd_file_t* f   = mpk->udata;
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_LIKELY(!mpk->busy)) {
    msgpack_unpacked_init(&ctx->upkd);
  }

  if (HEDLEY_UNLIKELY(!upd_msgpack_pop(mpk, &ctx->upkd))) {
    msgpack_unpacked_destroy(&ctx->upkd);
    if (HEDLEY_UNLIKELY(mpk->broken)) {
      upd_file_trigger(f, UPD_FILE_UPDATE);
    }
    if (mpk->busy) {
      mpk->busy = 0;
      upd_file_unref(f);
    }
    return;
  }

  if (HEDLEY_LIKELY(!mpk->busy)) {
    mpk->busy = true;
    upd_file_ref(f);
  }

  ctx->par = (upd_proto_parse_t) {
    .iso   = f->iso,
    .src   = &ctx->upkd.data,
    .iface = UPD_PROTO_ENCODER,
    .udata = f,
    .cb    = stream_proto_parse_cb_,
  };
  upd_proto_parse(&ctx->par);
}

static void stream_proto_parse_cb_(upd_proto_parse_t* par) {
  upd_file_t*            f   = par->udata;
  stream_t_*             ctx = f->ctx;
  const upd_proto_msg_t* msg = &par->msg;

  if (HEDLEY_UNLIKELY(par->err)) {
    stream_error_(f, par->err);
    return;
  }

  msgpack_packer* pk = &ctx->mpk.pk;

  switch (msg->cmd) {
  case UPD_PROTO_ENCODER_INFO:
    ctx->mpk.broken |=
      msgpack_pack_map(pk, 2)                                ||
        upd_msgpack_pack_cstr(pk, "success")                 ||
          msgpack_pack_true(pk)                              ||
        upd_msgpack_pack_cstr(pk, "result")                  ||
          msgpack_pack_map(pk, 4)                            ||
            upd_msgpack_pack_cstr(pk, "description")         ||
              upd_msgpack_pack_cstr(pk, "PNG image encoder") ||
            upd_msgpack_pack_cstr(pk, "type")                ||
              upd_msgpack_pack_cstr(pk, "image")             ||
            upd_msgpack_pack_cstr(pk, "initParam")           ||
              msgpack_pack_map(pk, 0)                        ||
            upd_msgpack_pack_cstr(pk, "frameParam")          ||
              msgpack_pack_map(pk, 0);
    upd_file_trigger(f, UPD_FILE_UPDATE);
    stream_msgpack_cb_(&ctx->mpk);
    return;

  case UPD_PROTO_ENCODER_INIT:
    stream_success_(f);
    return;

  case UPD_PROTO_ENCODER_FRAME: {
    const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = msg->encoder_frame.file,
        .udata = f,
        .cb    = stream_lock_tensor_cb_,
      });
    if (HEDLEY_UNLIKELY(!lock)) {
      stream_error_(f, "tensor lock refusal");
      return;
    }
  } return;

  case UPD_PROTO_ENCODER_FINALIZE:
    stream_success_(f);
    return;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }
}

static void stream_lock_tensor_cb_(upd_file_lock_t* k) {
  upd_file_t* f      = k->udata;
  upd_iso_t*  iso    = f->iso;
  stream_t_*  ctx    = f->ctx;
  upd_file_t* tensor = k->file;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    stream_error_(f, "tensor lock cancelled");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(ctx->target == tensor)) {
    stream_success_(f);  /* src == dst */
    goto ABORT;
  }

  const bool req = upd_req_with_dup(&(upd_req_t) {
      .file  = tensor,
      .type  = UPD_REQ_TENSOR_DATA,
      .tensor = { .meta = {
        .type = UPD_TENSOR_U16,
      }, },
      .udata = k,
      .cb    = stream_tensor_data_cb_,
    });
  if (HEDLEY_UNLIKELY(!req)) {
    stream_error_(f, "tensor data req refusal");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void stream_tensor_data_cb_(upd_req_t* req) {
  upd_file_lock_t* k      = req->udata;
  upd_file_t*      f      = k->udata;
  upd_iso_t*       iso    = f->iso;
  stream_t_*       ctx    = f->ctx;
  upd_file_t*      tensor = k->file;

  const upd_req_tensor_data_t data = req->tensor.data;
  /* req pointer is reused for FLUSH request */

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    upd_iso_unstack(iso, req);
    upd_file_unlock(k);
    upd_iso_unstack(iso, k);

    stream_error_(f, "tensor data req failure");
    return;
  }

  if (HEDLEY_UNLIKELY(data.meta.rank != 3)) {
    stream_error_(f, "incompatible tensor rank");
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(data.meta.reso[0] == 0 || 4 < data.meta.reso[0])) {
    stream_error_(f, "resolution of 1st rank must be 1~4");
    goto ABORT;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = ctx->target,
      .ex    = true,
      .udata = req,
      .cb    = stream_lock_png_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    stream_error_(f, "png file lock refusal");
    goto ABORT;
  }
  return;

ABORT:
  /* forks from main program (Do not call stream_error_ anymore!) */
  *req = (upd_req_t) {
    .file  = tensor,
    .type  = UPD_REQ_TENSOR_FLUSH,
    .udata = k,
    .cb    = stream_flush_tensor_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(req))) {
    stream_flush_tensor_cb_(req);
  }
}

static void stream_lock_png_cb_(upd_file_lock_t* k) {
  upd_req_t*       req    = k->udata;
  upd_file_lock_t* tk     = req->udata;
  upd_file_t*      f      = tk->udata;
  upd_iso_t*       iso    = f->iso;
  stream_t_*       ctx    = f->ctx;
  png_t_*          png    = ctx->target->ctx;
  upd_file_t*      tensor = tk->file;

  bool abort = true;

  const upd_req_tensor_data_t* data = &req->tensor.data;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    stream_error_(f, "png file lock cancelled");
    goto EXIT;
  }

  png->clean  = true;
  png->broken = true;

  png->depth = data->meta.type == UPD_TENSOR_U8? 1: 2;

  png->spp = data->meta.reso[0];
  png->w   = data->meta.reso[1];
  png->h   = data->meta.reso[2];

  const size_t n    = upd_tensor_count_scalars(&data->meta);
  const size_t size = n*png->depth;

  if (HEDLEY_UNLIKELY(!upd_malloc(&png->data, size))) {
    stream_error_(f, "pixbuf allocation failure");
    goto EXIT;
  }
  switch (data->meta.type) {
  case UPD_TENSOR_U8:
  case UPD_TENSOR_U16:
    memcpy(png->data, data->ptr, size);
    break;
  case UPD_TENSOR_F32:
    upd_tensor_conv_f32_to_u16((uint16_t*) png->data, (float*) data->ptr, n);
    break;
  case UPD_TENSOR_F64:
    upd_tensor_conv_f64_to_u16((uint16_t*) png->data, (double*) data->ptr, n);
    break;
  default:
    stream_error_(f, "unknown tensor type");
    goto EXIT;
  }

  k->udata = f;
  const bool wr = writer_start_with_dup_(&(io_t_) {
      .bin = ctx->target->backend,
      .writer = {
        .ihdr = {
          .width  = png->w,
          .height = png->h,
          .depth  = png->depth*8,
          .colortype =
            png->spp == 1? PNG_GRAYSCALE:
            png->spp == 2? PNG_GRAYSCALE_ALPHA:
            png->spp == 3? PNG_TRUECOLOR:
            png->spp == 4? PNG_TRUECOLOR_ALPHA: 0,
        },
        .data = png->data,
      },
      .udata = k,
      .cb    = stream_write_cb_,
    });
  if (HEDLEY_UNLIKELY(!wr)) {
    stream_error_(f, "writer start failure");
    goto EXIT;
  }
  abort = false;

EXIT:
  if (HEDLEY_UNLIKELY(abort)) {
    upd_file_unlock(k);
    upd_iso_unstack(iso, k);
  }

  /* forks from main program (Do not call stream_error_ anymore!) */
  *req = (upd_req_t) {
    .file  = tensor,
    .type  = UPD_REQ_TENSOR_FLUSH,
    .udata = tk,
    .cb    = stream_flush_tensor_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(req))) {
    stream_flush_tensor_cb_(req);
  }
}

static void stream_flush_tensor_cb_(upd_req_t* req) {
  upd_file_lock_t* k   = req->udata;
  upd_iso_t*       iso = k->file->iso;

  upd_iso_unstack(iso, req);
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void stream_write_cb_(io_t_* io) {
  upd_file_lock_t* k   = io->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  stream_t_*       ctx = f->ctx;
  png_t_*          png = ctx->target->ctx;

  png->broken = !io->ok;

  const char* msg = io->msg;
  upd_iso_unstack(iso, io);

  upd_file_unlock(k);
  upd_iso_unstack(iso, k);

  if (HEDLEY_LIKELY(!png->broken)) {
    ctx->target->cache = png->depth * png->spp * png->w * png->h;
    stream_success_(f);
  } else {
    ctx->target->cache = 0;
    stream_error_(f, msg);
  }
}


static void reader_lock_bin_cb_(upd_file_lock_t* k) {
  io_t_* io = k->udata;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    io->msg = "backend lock cancelled";
    goto ABORT;
  }
  io->locked = true;

  io->req = (upd_req_t) {
    .file = io->bin,
    .type = UPD_REQ_STREAM_READ,
    .stream = { .io = {
      .size = SIZE_MAX,
    }, },
    .udata = io,
    .cb    = reader_read_bin_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&io->req))) {
    io->msg = "backend read refusal";
    goto ABORT;
  }
  return;

ABORT:
  reader_finalize_(io);
}

static void reader_read_bin_cb_(upd_req_t* req) {
  io_t_* io = req->udata;

  const upd_req_stream_io_t* sio = &req->stream.io;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    io->msg = "backend file read failure";
    goto FINALIZE;
  }
  if (HEDLEY_UNLIKELY(sio->size == 0)) {
    io->msg = "binary data ends unexpectedly";
    goto FINALIZE;
  }

  const size_t used = png_reader_consume(&io->reader, sio->buf, sio->size);
  if (HEDLEY_UNLIKELY(used == 0)) {
    io->msg = "binary data is too short";
    goto FINALIZE;
  }

  io->offset += used;
  switch (io->reader.state) {
  case PNG_READER_STATE_DONE:
    io->ok = true;
    goto FINALIZE;

  case PNG_READER_STATE_ERROR:
    io->msg = io->reader.msg;
    goto FINALIZE;

  default:
    io->req = (upd_req_t) {
      .file = io->bin,
      .type = UPD_REQ_STREAM_READ,
      .stream = { .io = {
        .size   = SIZE_MAX,
        .offset = io->offset,
      }, },
      .udata = io,
      .cb    = reader_read_bin_cb_,
    };
    if (HEDLEY_UNLIKELY(!upd_req(&io->req))) {
      io->msg = "continuous backend read refusal";
      goto FINALIZE;
    }
    return;
  }

FINALIZE:
  reader_finalize_(io);
}


static void writer_lock_bin_cb_(upd_file_lock_t* k) {
  io_t_* io = k->udata;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    io->msg = "backend lock cancelled";
    goto ABORT;
  }
  io->locked = true;

  io->req = (upd_req_t) {
    .file   = io->bin,
    .result = UPD_REQ_OK,
    .udata  = io,
  };
  writer_write_bin_cb_(&io->req);
  return;

ABORT:
  writer_finalize_(io);
}

static void writer_write_bin_cb_(upd_req_t* req) {
  io_t_* io = req->udata;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    io->msg = "backend file write failure";
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(io->writer.state == PNG_WRITER_STATE_DONE)) {
    io->req = (upd_req_t) {
      .file = io->bin,
      .type = UPD_REQ_STREAM_TRUNCATE,
      .stream = { .io = {
        .size = io->offset,
      }, },
      .udata  = io,
      .cb     = writer_truncate_bin_cb_,
    };
    if (HEDLEY_UNLIKELY(!upd_req(&io->req))) {
      io->msg = "backend file truncate refusal";
      goto ABORT;
    }
    return;
  }

  const size_t wrote = png_writer_write(&io->writer, io->buf, io->buflen);
  if (HEDLEY_UNLIKELY(io->writer.state == PNG_WRITER_STATE_ERROR)) {
    io->msg = io->writer.msg;
    goto ABORT;
  }
  assert(wrote > 0);

  io->req = (upd_req_t) {
    .file   = io->bin,
    .type   = UPD_REQ_STREAM_WRITE,
    .stream = { .io = {
      .offset = io->offset,
      .buf    = io->buf,
      .size   = wrote,
    }, },
    .udata  = io,
    .cb     = writer_write_bin_cb_,
  };
  io->offset += wrote;
  if (HEDLEY_UNLIKELY(!upd_req(&io->req))) {
    io->msg = "backend file write refusal";
    goto ABORT;
  }
  return;

ABORT:
  writer_finalize_(io);
}

static void writer_truncate_bin_cb_(upd_req_t* req) {
  io_t_* io = req->udata;

  if (HEDLEY_LIKELY(req->result == UPD_REQ_OK)) {
    io->ok = true;
  } else {
    io->msg = "backend file truncate failure";
  }
  writer_finalize_(io);
}
