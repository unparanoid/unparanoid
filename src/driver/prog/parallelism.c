#include "common.h"


/* This program works well on only little-endian machine. =) */


#define SESSION_BUFFER_MAX_ (1024*1024*8)  /* = 8 MiB */
#define OUTPUT_BUFFER_MAX_  (1024*1024*8)  /* = 8 MiB */


typedef struct ctx_t_     ctx_t_;
typedef struct session_t_ session_t_;


struct ctx_t_ {
  upd_file_t* file;

  upd_array_of(session_t_*) sessions;

  session_t_* updated;

  upd_buf_t buf;
};

struct session_t_ {
  uint16_t id;
  ctx_t_*  ctx;

  upd_file_t*      prog;
  upd_file_t*      io;
  upd_file_watch_t watch;

  size_t    parsing;
  upd_buf_t buf;
};


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

const upd_driver_t upd_driver_prog_parallelism = {
  .name = (uint8_t*) "upd.prog.parallelism",
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

static const upd_driver_t stream_driver_ = {
  .name = (uint8_t*) "upd.prog.parallelism.stream",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_STREAM,
    0,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


static
bool
stream_input_(
  upd_req_t* req);

static
void
stream_output_pipe_(
  ctx_t_*        ctx,
  uint16_t       id,
  const uint8_t* buf,
  size_t         n);


static
void
stream_add_session_(
  ctx_t_*        ctx,
  uint16_t       id,
  const uint8_t* name,
  size_t         len);

static
session_t_*
stream_find_session_(
  ctx_t_*  ctx,
  uint16_t id);


static
void
session_delete_(
  session_t_* ss);

static
bool
session_input_pipe_(
  session_t_* ss);


static
void
session_find_cb_(
  upd_req_pathfind_t* pf);

static
void
session_lock_for_exec_cb_(
  upd_file_lock_t* lock);

static
void
session_exec_cb_(
  upd_req_t* req);

static
void
session_lock_for_input_cb_(
  upd_file_lock_t* lock);

static
void
session_input_cb_(
  upd_req_t* req);

static
void
session_watch_cb_(
  upd_file_watch_t* w);

static
void
session_lock_for_output_cb_(
  upd_file_lock_t* lock);

static
void
session_output_cb_(
  upd_req_t* req);


static bool prog_init_(upd_file_t* f) {
  (void) f;
  return true;
}

static void prog_deinit_(upd_file_t* f) {
  (void) f;
}

static bool prog_handle_(upd_req_t* req) {
  upd_iso_t* iso = req->file->iso;

  switch (req->type) {
  case UPD_REQ_PROG_ACCESS:
    req->prog.access = (upd_req_prog_access_t) {
      .exec = true,
    };
    break;
  case UPD_REQ_PROG_EXEC: {
    upd_file_t* f = upd_file_new(iso, &stream_driver_);
    if (HEDLEY_UNLIKELY(f == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    req->prog.exec = f;
    req->result = UPD_REQ_OK;
    req->cb(req);
    upd_file_unref(f);
  } return true;
  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}


static bool stream_init_(upd_file_t* f) {
  ctx_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (ctx_t_) {
    .file = f,
    .buf  = { .max = OUTPUT_BUFFER_MAX_, },
  };
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  ctx_t_* ctx = f->ctx;
  while (ctx->sessions.n) {
    session_delete_(ctx->sessions.p[0]);
  }
  upd_free(&ctx);
}

static bool stream_handle_(upd_req_t* req) {
  ctx_t_* ctx = req->file->ctx;

  switch (req->type) {
  case UPD_REQ_STREAM_ACCESS:
    req->stream.access = (upd_req_stream_access_t) {
      .input  = true,
      .output = true,
    };
    break;

  case UPD_REQ_STREAM_INPUT:
    if (HEDLEY_UNLIKELY(!stream_input_(req))) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    break;

  case UPD_REQ_STREAM_OUTPUT:
    req->stream.io = (upd_req_stream_io_t) {
      .size = ctx->buf.size,
      .buf  = ctx->buf.ptr,
    };
    upd_buf_t oldbuf = ctx->buf;
    ctx->buf = (upd_buf_t) {0};
    req->result = UPD_REQ_OK;
    req->cb(req);
    upd_buf_clear(&oldbuf);
    return true;

  default:
    return false;
  }
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}


static bool stream_input_(upd_req_t* req) {
  upd_iso_t* iso = req->file->iso;
  ctx_t_*    ctx = req->file->ctx;

  upd_req_stream_io_t* io = &req->stream.io;

  size_t         rem = io->size;
  const uint8_t* buf = io->buf;

  while (rem >= 4) {
    const uint8_t id = (buf[1] << 8) | buf[0];
    const uint8_t sz = (buf[3] << 8) | buf[2];

    const size_t whole = 4 + sz;
    if (HEDLEY_UNLIKELY(rem < whole)) {
      break;
    }

    session_t_* ss = stream_find_session_(ctx, id);
    if (HEDLEY_UNLIKELY(!ss)) {
      stream_add_session_(ctx, id, buf+4, sz);
      goto SKIP;
    }
    if (HEDLEY_UNLIKELY(sz == 0)) {
      session_delete_(ss);
      goto SKIP;
    }

    if (HEDLEY_UNLIKELY(!upd_buf_append(&ss->buf, buf+4, sz))) {
      upd_iso_msgf(iso, "session buffer allocation failure\n");
      session_delete_(ss);
      goto SKIP;
    }
    if (HEDLEY_UNLIKELY(!session_input_pipe_(ss))) {
      session_delete_(ss);
      goto SKIP;
    }

SKIP:
    buf += whole;
    rem -= whole;
  }
  io->size = io->size - rem;
  return true;
}

static void stream_output_pipe_(
    ctx_t_* ctx, uint16_t id, const uint8_t* buf, size_t n) {
  if (HEDLEY_UNLIKELY(n > UINT16_MAX)) {
    while (n) {
      const size_t part = n > UINT16_MAX? UINT16_MAX: n;
      stream_output_pipe_(ctx, id, buf, part);
      n -= part;
    }
    return;
  }

  const size_t prev_size = ctx->buf.size;

  const bool append =
    upd_buf_append(&ctx->buf, (uint8_t*) &id, 2) &&
    upd_buf_append(&ctx->buf, (uint8_t*) &n,  2) &&
    upd_buf_append(&ctx->buf, buf, n);
  if (HEDLEY_UNLIKELY(!append)) {
    upd_buf_drop_tail(&ctx->buf, ctx->buf.size - prev_size);
    upd_iso_msgf(ctx->file->iso, "parallelism stream buffer overflow\n");
    return;
  }
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);
}


static void stream_add_session_(
    ctx_t_* ctx, uint16_t id, const uint8_t* name, size_t len) {
  if (HEDLEY_UNLIKELY(stream_find_session_(ctx, id))) {
    goto ABORT;
  }

  session_t_* ss = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ss, sizeof(*ss)))) {
    goto ABORT;
  }
  *ss = (session_t_) {
    .id  = id,
    .ctx = ctx,
  };

  upd_file_ref(ctx->file);
  const bool ok = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso   = ctx->file->iso,
      .path  = (uint8_t*) name,
      .len   = utf8nsize_lazy(name, len),
      .udata = ss,
      .cb    = session_find_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_file_unref(ctx->file);
    upd_free(&ss);
    goto ABORT;
  }
  return;

ABORT:
  stream_output_pipe_(ctx, id, NULL, 0);
}

static session_t_* stream_find_session_(ctx_t_* ctx, uint16_t id) {
  for (size_t i = 0; i < ctx->sessions.n; ++i) {
    session_t_* ss = ctx->sessions.p[i];
    if (HEDLEY_UNLIKELY(ss->id == id)) {
      return ss;
    }
  }
  return NULL;
}


static void session_delete_(session_t_* ss) {
  if (HEDLEY_UNLIKELY(!upd_array_find_and_remove(&ss->ctx->sessions, ss))) {
    return;
  }

  stream_output_pipe_(ss->ctx, ss->id, NULL, 0);
  upd_file_unwatch(&ss->watch);
  upd_buf_clear(&ss->buf);
  upd_file_unref(ss->io);
  upd_file_unref(ss->prog);
  upd_free(&ss);
}

static bool session_input_pipe_(session_t_* ss) {
  if (HEDLEY_UNLIKELY(ss->parsing || !ss->buf.size)) {
    return true;
  }
  ss->parsing = ss->buf.size;

  upd_file_ref(ss->ctx->file);
  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = ss->io,
      .ex    = true,
      .udata = ss,
      .cb    = session_lock_for_input_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unref(ss->ctx->file);
    return false;
  }
  return true;
}


static void session_find_cb_(upd_req_pathfind_t* pf) {
  session_t_* ss  = pf->udata;
  ctx_t_*     ctx = ss->ctx;

  ss->prog = pf->len? NULL: pf->base;
  upd_iso_unstack(pf->iso, pf);

  if (HEDLEY_UNLIKELY(ss->prog == NULL)) {
    goto ABORT;
  }

  upd_file_ref(ss->prog);
  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = ss->prog,
      .udata = ss,
      .cb    = session_lock_for_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unref(ss->prog);
    goto ABORT;
  }
  return;

ABORT:
  stream_output_pipe_(ss->ctx, ss->id, NULL, 0);
  upd_free(&ss);
  upd_file_unref(ctx->file);
}

static void session_lock_for_exec_cb_(upd_file_lock_t* lock) {
  session_t_* ss  = lock->udata;
  ctx_t_*     ctx = ss->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }

  const bool exec = upd_req_with_dup(&(upd_req_t) {
      .file  = ss->prog,
      .type  = UPD_REQ_PROG_EXEC,
      .udata = lock,
      .cb    = session_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!exec)) {
    goto ABORT;
  }
  return;

ABORT:
  stream_output_pipe_(ss->ctx, ss->id, NULL, 0);
  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);
  upd_file_unref(ss->prog);
  upd_free(&ss);
  upd_file_unref(ctx->file);
}

static void session_exec_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  session_t_*      ss   = lock->udata;
  ctx_t_*          ctx  = ss->ctx;

  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  ss->io = req->prog.exec;
  upd_iso_unstack(ctx->file->iso, req);

  if (HEDLEY_UNLIKELY(ss->io == NULL)) {
    goto ABORT;
  }
  upd_file_ref(ss->io);

  ss->watch = (upd_file_watch_t) {
    .file  = ss->io,
    .udata = ss,
    .cb    = session_watch_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&ss->watch))) {
    upd_file_unref(ss->io);
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->sessions, ss, SIZE_MAX))) {
    upd_file_unwatch(&ss->watch);
    upd_file_unref(ss->io);
    goto ABORT;
  }
  upd_file_unref(ctx->file);
  return;

ABORT:
  stream_output_pipe_(ss->ctx, ss->id, NULL, 0);
  upd_file_unref(ss->prog);
  upd_free(&ss);
  upd_file_unref(ctx->file);
}

static void session_lock_for_input_cb_(upd_file_lock_t* lock) {
  session_t_* ss  = lock->udata;
  ctx_t_*     ctx = ss->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }

  const bool ok = upd_req_with_dup(&(upd_req_t) {
      .file = ss->io,
      .type = UPD_REQ_STREAM_INPUT,
      .stream = { .io = {
        .buf  = ss->buf.ptr,
        .size = ss->buf.size,
      }, },
      .udata = lock,
      .cb    = session_input_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  session_delete_(ss);
  upd_file_unref(ctx->file);
}

static void session_input_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  session_t_*      ss   = lock->udata;
  ctx_t_*          ctx  = ss->ctx;

  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  const upd_req_result_t    result = req->result;
  const upd_req_stream_io_t io     = req->stream.io;
  upd_iso_unstack(ctx->file->iso, req);

  if (HEDLEY_UNLIKELY(result != UPD_REQ_OK)) {
    session_delete_(ss);
    goto EXIT;
  }

  const bool retry = ss->parsing != ss->buf.size;

  ss->parsing   = 0;
  ss->buf.size -= io.size;
  memmove(ss->buf.ptr, ss->buf.ptr+io.size, ss->buf.size);

  if (HEDLEY_UNLIKELY(retry)) {
    if (HEDLEY_UNLIKELY(!session_input_pipe_(ss))) {
      session_delete_(ss);
      goto EXIT;
    }
  }

EXIT:
  upd_file_unref(ctx->file);
}

static void session_watch_cb_(upd_file_watch_t* w) {
  session_t_* ss  = w->udata;
  ctx_t_*     ctx = ss->ctx;

  switch (w->event) {
  case UPD_FILE_DELETE:
    assert(false);
    HEDLEY_UNREACHABLE();
    return;

  case UPD_FILE_UPDATE: {
    upd_file_ref(ctx->file);
    const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = ss->io,
        .ex    = true,
        .udata = ss,
        .cb    = session_lock_for_output_cb_,
      });
    if (HEDLEY_UNLIKELY(!lock)) {
      upd_file_unref(ctx->file);
      session_delete_(ss);
      return;
    }
  } return;
  }
}

static void session_lock_for_output_cb_(upd_file_lock_t* lock) {
  session_t_* ss  = lock->udata;
  ctx_t_*     ctx = ss->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }
  const bool output = upd_req_with_dup(&(upd_req_t) {
      .file  = ss->io,
      .type  = UPD_REQ_STREAM_OUTPUT,
      .udata = lock,
      .cb    = session_output_cb_,
    });
  if (HEDLEY_UNLIKELY(!output)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);
  session_delete_(ss);
  upd_file_unref(ctx->file);
}

static void session_output_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  session_t_*      ss   = lock->udata;
  ctx_t_*          ctx  = ss->ctx;

  const upd_req_result_t    result = req->result;
  const upd_req_stream_io_t io     = req->stream.io;
  upd_iso_unstack(ctx->file->iso, req);

  if (HEDLEY_LIKELY(result == UPD_REQ_OK)) {
    if (HEDLEY_LIKELY(io.size)) {
      stream_output_pipe_(ctx, ss->id, io.buf, io.size);
    }
  } else {
    session_delete_(ss);
  }
  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  upd_file_unref(ctx->file);

}
