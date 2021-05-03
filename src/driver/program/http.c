#include "common.h"


#define WSOCK_NONCE_IN_SIZE_  24
#define WSOCK_NONCE_OUT_SIZE_ 28
#define WSOCK_NONCE_PREFIX_   "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


typedef enum http_state_t_ {
  REQUEST_,
  RESPONSE_,
  WSOCK_,
  END_,
} http_state_t_;


typedef struct http_t_  http_t_;
typedef struct req_t_   req_t_;

struct http_t_ {
  upd_file_t*   file;
  http_state_t_ state;
  upd_buf_t     out;

  upd_file_t*      ws;
  upd_file_watch_t wswatch;
  upd_buf_t        wsbuf;
};

struct req_t_ {
  http_t_*   ctx;
  upd_req_t* req;

  uint8_t* method;
  size_t   method_len;

  uint8_t* path;
  size_t   path_len;

  int minor_version;

  struct phr_header headers[64];
  size_t headers_cnt;

  upd_file_t* file;
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

const upd_driver_t upd_driver_program_http = {
  .name = (uint8_t*) "upd.program.http",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_PROGRAM,
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
  .name = (uint8_t*) "upd.program.http.stream",
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
stream_output_wsock_(
  http_t_*       ctx,
  const wsock_t* ws,
  const uint8_t* body);

static
void
stream_end_(
  http_t_* ctx);

static
bool
stream_parse_req_(
  http_t_*   ctx,
  upd_req_t* req);

static
bool
stream_pipe_wsock_input_(
  http_t_*   ctx,
  upd_req_t* req);

static
bool
stream_output_http_error_(
  http_t_*    ctx,
  uint16_t    code,
  const char* msg);


static
void
req_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
req_bin_access_cb_(
  upd_req_t* req);

static
void
req_lock_for_read_cb_(
  upd_file_lock_t* lock);

static
void
req_read_cb_(
  upd_req_t* req);

static
void
req_lock_for_exec_cb_(
  upd_file_lock_t* lock);

static
void
req_exec_cb_(
  upd_req_t* req);


static
const struct phr_header*
req_find_header_(
  const req_t_* req,
  const char*   name);

static
bool
req_calc_wsock_nonce_(
  uint8_t       out[WSOCK_NONCE_OUT_SIZE_],
  const req_t_* req);


static
void
wsock_lock_for_input_cb_(
  upd_file_lock_t* lock);

static
void
wsock_input_cb_(
  upd_req_t* req);

static
void
wsock_watch_cb_(
  upd_file_watch_t* w);


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
  case UPD_REQ_PROGRAM_ACCESS:
    req->program.access = (upd_req_program_access_t) {
      .exec = true,
    };
    break;
  case UPD_REQ_PROGRAM_EXEC: {
    upd_file_t* f = upd_file_new(iso, &stream_driver_);
    if (HEDLEY_UNLIKELY(f == NULL)) {
      return false;
    }
    req->program.exec = f;
  } break;
  default:
    return false;
  }
  req->cb(req);
  return true;
}


static bool stream_init_(upd_file_t* f) {
  http_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (http_t_) {
    .file = f,
  };
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  http_t_* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->ws)) {
    upd_file_unref(ctx->ws);
  }
  upd_buf_clear(&ctx->wsbuf);

  upd_buf_clear(&ctx->out);
  upd_free(&ctx);
}

static bool stream_handle_(upd_req_t* req) {
  http_t_* ctx = req->file->ctx;

  switch (req->type) {
  case UPD_REQ_STREAM_ACCESS:
    req->stream.access = (upd_req_stream_access_t) {
      .input  = true,
      .output = true,
    };
    break;

  case UPD_REQ_STREAM_INPUT:
    switch (ctx->state) {
    case REQUEST_:
      return stream_parse_req_(ctx, req);
    case RESPONSE_:
      req->stream.io.size = 0;
      return true;
    case WSOCK_:
      return stream_pipe_wsock_input_(ctx, req);
    default:
      return false;
    }

  case UPD_REQ_STREAM_OUTPUT: {
    if (HEDLEY_UNLIKELY(ctx->state == END_ && !ctx->out.size)) {
      return false;
    }
    req->stream.io = (upd_req_stream_io_t) {
      .buf  = ctx->out.ptr,
      .size = ctx->out.size,
    };
    upd_buf_t oldbuf = ctx->out;
    ctx->out = (upd_buf_t) {0};

    req->cb(req);
    upd_buf_clear(&oldbuf);
  } return true;

  default:
    return false;
  }
  req->cb(req);
  return true;
}


static bool stream_output_wsock_(
    http_t_* ctx, const wsock_t* ws, const uint8_t* body) {
  const size_t header = wsock_encode_size(ws);
  const size_t whole  = header + ws->payload_len;

  uint8_t* ptr = upd_buf_append(&ctx->out, NULL, whole);
  if (HEDLEY_UNLIKELY(ptr == NULL)) {
    return false;
  }
  wsock_encode(ptr, ws);
  memcpy(ptr+header, body, ws->payload_len);
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);
  return true;
}

static void stream_end_(http_t_* ctx) {
  if (HEDLEY_LIKELY(ctx->state != END_)) {
    ctx->state = END_;
    upd_file_trigger(ctx->file, UPD_FILE_UPDATE);
  }
}

static bool stream_parse_req_(http_t_* ctx, upd_req_t* req) {
  upd_req_stream_io_t* io = &req->stream.io;

  req_t_* hreq = upd_iso_stack(ctx->file->iso, sizeof(*hreq));
  if (HEDLEY_UNLIKELY(hreq == NULL)) {
    return false;
  }
  *hreq = (req_t_) {
    .ctx         = ctx,
    .req         = req,
    .headers_cnt = sizeof(hreq->headers)/sizeof(hreq->headers[0]),
  };

  const int result = phr_parse_request(
    (char*) io->buf, io->size,
    (const char**) &hreq->method, &hreq->method_len,
    (const char**) &hreq->path, &hreq->path_len,
    &hreq->minor_version,
    hreq->headers, &hreq->headers_cnt, 0);

  if (HEDLEY_UNLIKELY(result == -1)) {
    upd_iso_unstack(ctx->file->iso, hreq);
    req->cb(req);
    return stream_output_http_error_(ctx, 400, "invalid request");
  }

  if (HEDLEY_LIKELY(result == -2)) {
    upd_iso_unstack(ctx->file->iso, hreq);
    io->size = 0;
    req->cb(req);
    return true;
  }

  io->size   = result;
  ctx->state = RESPONSE_;

  const bool is_get =
    hreq->method_len == 3 &&
    utf8ncasecmp(hreq->method, "GET", 3) == 0;
  if (HEDLEY_UNLIKELY(!is_get)) {
    upd_iso_unstack(ctx->file->iso, hreq);
    req->cb(req);
    return stream_output_http_error_(ctx, 405, "unknown method");
  }

  upd_file_ref(ctx->file);
  const bool pathfind = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso   = ctx->file->iso,
      .path  = hreq->path,
      .len   = hreq->path_len,
      .udata = hreq,
      .cb    = req_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!pathfind)) {
    upd_file_unref(ctx->file);
    upd_iso_unstack(ctx->file->iso, hreq);
    req->cb(req);
    return stream_output_http_error_(ctx, 500, "pathfind failure");
  }
  return true;
}

static bool stream_pipe_wsock_input_(http_t_* ctx, upd_req_t* req) {
  upd_file_ref(ctx->file);
  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = ctx->ws,
      .ex    = true,
      .udata = req,
      .cb    = wsock_lock_for_input_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unref(ctx->file);
    return false;
  }
  return true;
}

static bool stream_output_http_error_(
    http_t_* ctx, uint16_t code, const char* msg) {
  uint8_t temp[1024] = {0};
  const size_t len = snprintf((char*) temp, sizeof(temp),
    "HTTP/1.1 %"PRIu16" %s\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "UNPARANOID HTTP stream error: %s (%"PRIu16")\r\n",
    code, msg, msg, code);

  const bool ret = upd_buf_append(&ctx->out, temp, len);
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);

  stream_end_(ctx);
  return ret;
}


static void req_pathfind_cb_(upd_req_pathfind_t* pf) {
  req_t_*  req = pf->udata;
  http_t_* ctx = req->ctx;

  req->file = pf->len? NULL: pf->base;
  upd_iso_unstack(ctx->file->iso, pf);

  if (HEDLEY_UNLIKELY(!req->file)) {
    stream_output_http_error_(ctx, 404, "not found");
    goto ABORT;
  }

  /* check if the client requests wsock */
  const struct phr_header* upgrade = req_find_header_(req, "Upgrade");
  if (HEDLEY_UNLIKELY(upgrade != NULL)) {
    const bool match =
      upgrade->value_len == 9 &&
      utf8ncasecmp(upgrade->value, "websocket", 9) == 0;
    if (HEDLEY_LIKELY(match)) {
      const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
          .file  = req->file,
          .udata = req,
          .cb    = req_lock_for_exec_cb_,
        });
      if (HEDLEY_UNLIKELY(!lock)) {
        stream_output_http_error_(ctx, 500, "lock context allocation failure");
        goto ABORT;
      }
      return;
    }
  }

  const bool access = upd_req_with_dup(&(upd_req_t) {
      .file  = req->file,
      .type  = UPD_REQ_BIN_ACCESS,
      .udata = req,
      .cb    = req_bin_access_cb_,
    });
  if (HEDLEY_UNLIKELY(!access)) {
    stream_output_http_error_(ctx, 403, "refused access request");
    goto ABORT;
  }
  return;

ABORT:
  req->req->cb(req->req);
  upd_iso_unstack(ctx->file->iso, req);
  upd_file_unref(ctx->file);
}

static void req_bin_access_cb_(upd_req_t* req) {
  req_t_*  hreq = req->udata;
  http_t_* ctx  = hreq->ctx;

  const bool readable = req->bin.access.read;
  upd_iso_unstack(ctx->file->iso, req);

  if (HEDLEY_UNLIKELY(!readable)) {
    stream_output_http_error_(ctx, 403, "file is not readable");
    goto ABORT;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = req->file,
      .udata = req,
      .cb    = req_lock_for_read_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    stream_output_http_error_(ctx, 500, "lock context allocation failure");
    goto ABORT;
  }
  return;

ABORT:
  hreq->req->cb(hreq->req);
  upd_iso_unstack(ctx->file->iso, hreq);
  upd_file_unref(ctx->file);
}

static void req_lock_for_read_cb_(upd_file_lock_t* lock) {
  req_t_*  req = lock->udata;
  http_t_* ctx = req->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    stream_output_http_error_(ctx, 409, "lock failure");
    goto ABORT;
  }

  uint8_t temp[1024];
  const size_t len = snprintf((char*) temp, sizeof(temp),
    "HTTP/1.1 200 OK\r\n"
    "Content-type: text/plain\r\n"
    "\r\n");

  const bool header = upd_buf_append(&ctx->out, temp, len);
  if (HEDLEY_UNLIKELY(!header)) {
    stream_output_http_error_(ctx, 500, "buffer allocation failure");
    goto ABORT;
  }
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);

  const bool read = upd_req_with_dup(&(upd_req_t) {
      .file  = req->file,
      .type  = UPD_REQ_BIN_READ,
      .bin   = { .rw = {
        .size = UINT64_MAX,
      }, },
      .udata = lock,
      .cb    = req_read_cb_,
    });
  if (HEDLEY_UNLIKELY(!read)) {
    stream_end_(ctx);
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  req->req->cb(req->req);
  upd_iso_unstack(ctx->file->iso, req);

  upd_file_unref(ctx->file);
}

static void req_read_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  req_t_*          hreq = lock->udata;
  http_t_*         ctx  = hreq->ctx;

  const upd_req_bin_rw_t rw = req->bin.rw;
  if (!rw.size) {
    goto FINALIZE;
  }

  if (HEDLEY_UNLIKELY(!upd_buf_append(&ctx->out, rw.buf, rw.size))) {
    upd_iso_msgf(ctx->file->iso,
      "HTTP response was too long, so we trimmed it! X(\n");
    goto FINALIZE;
  }
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);

  *req = (upd_req_t) {
      .file  = req->file,
      .type  = UPD_REQ_BIN_READ,
      .bin   = { .rw = {
        .offset = rw.offset + rw.size,
        .size   = UINT64_MAX,
      }, },
      .udata = lock,
      .cb    = req_read_cb_,
    };
  if (HEDLEY_UNLIKELY(!upd_req(req))) {
    goto FINALIZE;
  }
  return;

FINALIZE:
  upd_iso_unstack(ctx->file->iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  hreq->req->cb(hreq->req);
  upd_iso_unstack(ctx->file->iso, hreq);

  stream_end_(ctx);
  upd_file_unref(ctx->file);
}

static void req_lock_for_exec_cb_(upd_file_lock_t* lock) {
  req_t_*  req = lock->udata;
  http_t_* ctx = req->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    stream_output_http_error_(ctx, 409, "lock failure");
    goto ABORT;
  }

  const bool exec = upd_req_with_dup(&(upd_req_t) {
      .file  = req->file,
      .type  = UPD_REQ_PROGRAM_EXEC,
      .udata = lock,
      .cb    = req_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!exec)) {
    stream_output_http_error_(ctx, 403, "refused exec request");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  req->req->cb(req->req);
  upd_iso_unstack(ctx->file->iso, req);

  upd_file_unref(ctx->file);
}

static void req_exec_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  req_t_*          hreq = lock->udata;
  http_t_*         ctx  = hreq->ctx;

  ctx->ws = req->program.exec;
  upd_iso_unstack(ctx->file->iso, req);

  if (HEDLEY_UNLIKELY(ctx->ws == NULL)) {
    stream_output_http_error_(ctx, 403, "exec failure");
    goto EXIT;
  }

  uint8_t nonce[WSOCK_NONCE_OUT_SIZE_+1] = {0};
  if (HEDLEY_UNLIKELY(!req_calc_wsock_nonce_(nonce, hreq))) {
    stream_output_http_error_(ctx, 400, "wsock nonce failure");
    goto EXIT;
  }

  ctx->wswatch = (upd_file_watch_t) {
    .file  = ctx->ws,
    .udata = ctx,
    .cb    = wsock_watch_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->wswatch))) {
    stream_output_http_error_(ctx, 403, "watch failure");
    goto EXIT;
  }

  uint8_t temp[1024];
  const size_t len = snprintf((char*) temp, sizeof(temp),
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: upgrade\r\n"
    "Sec-WebSocket-Accept: %s\r\n"
    "\r\n", nonce);

  const bool header = upd_buf_append(&ctx->out, temp, len);
  if (HEDLEY_UNLIKELY(!header)) {
    stream_output_http_error_(ctx, 500, "buffer allocation failure");
    goto EXIT;
  }
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);

  ctx->state = WSOCK_;

EXIT:
  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  hreq->req->cb(hreq->req);
  upd_iso_unstack(ctx->file->iso, hreq);

  upd_file_unref(ctx->file);
}


static const struct phr_header* req_find_header_(
    const req_t_* req, const char* name) {
  const size_t namelen = utf8size_lazy(name);

  for (size_t i = 0; i < req->headers_cnt; ++i) {
    const struct phr_header* h = &req->headers[i];
    const bool match =
      h->name_len == namelen && utf8ncmp(h->name, name, namelen) == 0;
    if (HEDLEY_UNLIKELY(match)) {
      return h;
    }
  }
  return NULL;
}

static bool req_calc_wsock_nonce_(
    uint8_t out[WSOCK_NONCE_OUT_SIZE_], const req_t_* req) {
  const struct phr_header* h = req_find_header_(req, "Sec-WebSocket-Key");
  if (HEDLEY_UNLIKELY(h == NULL || h->value_len != WSOCK_NONCE_IN_SIZE_)) {
    return NULL;
  }

  uint8_t hashed[SHA1_BLOCK_SIZE];
  SHA1_CTX sha1;
  sha1_init(&sha1);
  sha1_update(&sha1, (uint8_t*) h->value, WSOCK_NONCE_IN_SIZE_);
  sha1_update(&sha1,
    (uint8_t*) WSOCK_NONCE_PREFIX_, sizeof(WSOCK_NONCE_PREFIX_)-1);
  sha1_final(&sha1, hashed);

  const size_t outsz = base64_encode(hashed, NULL, sizeof(hashed), false);
  if (HEDLEY_UNLIKELY(outsz != WSOCK_NONCE_OUT_SIZE_)) {
    return false;
  }
  base64_encode(hashed, out, sizeof(hashed), false);
  return true;
}


static void wsock_lock_for_input_cb_(upd_file_lock_t* lock) {
  upd_req_t*           req = lock->udata;
  http_t_*             ctx = req->file->ctx;
  upd_req_stream_io_t* io  = &req->stream.io;

  const size_t prev_size = ctx->wsbuf.size;

  if (HEDLEY_UNLIKELY(!lock->ok || ctx->state != WSOCK_)) {
    goto EXIT;
  }

  const uint8_t* buf = io->buf;
  bool           end = false;

  while (io->size) {
    wsock_t w = {0};

    const size_t header = wsock_decode(&w, buf, io->size);
    if (!header || io->size < header+w.payload_len) {
      break;
    }

    const uint8_t* body  = buf + header;
    const size_t   whole = header + w.payload_len;

    buf      += whole;
    io->size -= whole;

    switch (w.opcode) {
    case WSOCK_OPCODE_CONT:
    case WSOCK_OPCODE_TEXT:
    case WSOCK_OPCODE_BIN: {
      if (HEDLEY_UNLIKELY(!upd_buf_append(&ctx->wsbuf, body, w.payload_len))) {
        end = true;
        goto EXIT;
      }
      if (HEDLEY_UNLIKELY(w.mask)) {
        const size_t head = ctx->wsbuf.size - w.payload_len;
        wsock_mask(ctx->wsbuf.ptr+head, w.payload_len, w.mask_key);
      }
    } break;

    case WSOCK_OPCODE_CLOSE:
      stream_output_wsock_(ctx, &(wsock_t) {
          .opcode = WSOCK_OPCODE_CLOSE,
          .fin    = true,
        }, NULL);
      end = true;
      goto EXIT;

    case WSOCK_OPCODE_PING: {
      const bool sent = stream_output_wsock_(ctx, &(wsock_t) {
          .payload_len = w.payload_len,
          .mask_key    = w.mask_key,
          .opcode      = WSOCK_OPCODE_PONG,
          .fin         = true,
          .mask        = w.mask,
        }, body);
      if (HEDLEY_UNLIKELY(!sent)) {
        end = true;
        goto EXIT;
      }
    } break;

    case WSOCK_OPCODE_PONG: {
      const bool sent = stream_output_wsock_(ctx, &(wsock_t) {
          .opcode = WSOCK_OPCODE_CLOSE,
          .fin    = true,
        }, NULL);
      if (HEDLEY_UNLIKELY(!sent)) {
        end = true;
        goto EXIT;
      }
    } break;

    default:
      end = true;
      goto EXIT;
    }
  }

  bool try_input;
EXIT:
  try_input = ctx->wsbuf.size != prev_size;
  if (HEDLEY_LIKELY(try_input)) {
    lock->udata = ctx;
    const bool input = upd_req_with_dup(&(upd_req_t) {
        .file = ctx->ws,
        .type = UPD_REQ_STREAM_INPUT,
        .stream = { .io = {
          .size = ctx->wsbuf.size,
          .buf  = ctx->wsbuf.ptr,
        }, },
        .udata = lock,
        .cb    = wsock_input_cb_,
      });
    if (HEDLEY_UNLIKELY(!input)) {
      try_input = false;
      end       = true;
    }
  }
  if (HEDLEY_UNLIKELY(end)) {
    stream_end_(ctx);
  }
  if (HEDLEY_UNLIKELY(!try_input)) {
    upd_file_unlock(lock);
    upd_iso_unstack(ctx->file->iso, lock);
    upd_file_unref(ctx->file);
  }
  req->cb(req);  /* We don't need io->buf anymore. :) */
}

static void wsock_input_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  http_t_*         ctx  = lock->udata;

  upd_buf_drop_head(&ctx->wsbuf, req->stream.io.size);
  upd_iso_unstack(ctx->file->iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  upd_file_unref(ctx->file);
}

static void wsock_watch_cb_(upd_file_watch_t* w) {
  http_t_* ctx = w->udata;

  if (HEDLEY_LIKELY(w->event == UPD_FILE_UPDATE)) {
    /* TODO */
    upd_iso_msgf(ctx->file->iso, "wsock output\n");
  }
}
