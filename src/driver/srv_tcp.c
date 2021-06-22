#include "common.h"


#define TCP_BACKLOG_ 255


typedef struct srv_t_ {
  uv_tcp_t tcp;

  upd_file_watch_t watch;

  upd_file_t* prog;
  uint16_t    port;

  unsigned running : 1;
} srv_t_;

typedef struct cli_t_ {
  uv_tcp_t      tcp;
  uv_shutdown_t shutdown;

  upd_file_watch_t watch;
  upd_file_watch_t watchst;

  upd_file_lock_t k;
  upd_file_t*     srv;
} cli_t_;


static
bool
srv_init_(
  upd_file_t* f);

static
void
srv_deinit_(
  upd_file_t* f);

static
bool
srv_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_srv_tcp = {
  .name   = (uint8_t*) "upd.srv.tcp",
  .cats   = (upd_req_cat_t[]) {0},
  .init   = srv_init_,
  .deinit = srv_deinit_,
  .handle = srv_handle_,
};


static
bool
cli_init_(
  upd_file_t* f);

static
void
cli_deinit_(
  upd_file_t* f);

static
bool
cli_handle_(
  upd_req_t* req);

static const upd_driver_t cli_ = {
  .name   = (uint8_t*) "upd.srv.tcp.cli_",
  .cats   = (upd_req_cat_t[]) {0},
  .init   = cli_init_,
  .deinit = cli_deinit_,
  .handle = cli_handle_,
};


static
bool
cli_pipe_stream_to_tcp_(
  upd_file_t* f);


static
void
srv_watch_cb_(
  upd_file_watch_t* w);

static
void
srv_conn_cb_(
  uv_stream_t* stream,
  int          status);

static
void
srv_close_cb_(
  uv_handle_t* handle);


static
void
cli_lock_prog_cb_(
  upd_file_lock_t* k);

static
void
cli_exec_cb_(
  upd_req_t* req);

static
void
cli_lock_stream_cb_(
  upd_file_lock_t* k);

static
void
cli_alloc_cb_(
  uv_handle_t* hadle,
  size_t       n,
  uv_buf_t*    buf);

static
void
cli_watch_cb_(
  upd_file_watch_t* w);

static
void
cli_watch_stream_cb_(
  upd_file_watch_t* w);

static
void
cli_tcp_read_cb_(
  uv_stream_t*    stream,
  ssize_t         n,
  const uv_buf_t* buf);

static
void
cli_tcp_write_cb_(
  uv_write_t* req,
  int         status);

static
void
cli_stream_read_cb_(
  upd_req_t* req);

static
void
cli_stream_write_cb_(
  upd_req_t* req);

static
void
cli_shutdown_cb_(
  uv_shutdown_t* req,
  int            status);

static
void
cli_close_cb_(
  uv_handle_t* handle);


upd_file_t* upd_driver_srv_tcp_new(
    upd_file_t* prog, const uint8_t* host, uint16_t port) {
  upd_iso_t* iso = prog->iso;

  struct sockaddr_in addr = {0};
  if (HEDLEY_UNLIKELY(0 > uv_ip4_addr((char*) host, port, &addr))) {
    upd_iso_msgf(iso, "invalid addr: %s:%"PRIu16"\n", host, port);
    return NULL;
  }

  upd_file_t* f = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = &upd_driver_srv_tcp,
    });
  if (HEDLEY_UNLIKELY(f == NULL)) {
    upd_iso_msgf(iso, "server file allocation failure\n");
    return NULL;
  }

  srv_t_* srv = f->ctx;
  srv->port = port;
  srv->prog = prog;
  upd_file_ref(srv->prog);

  const int bind = uv_tcp_bind(&srv->tcp, (struct sockaddr*) &addr, 0);
  if (HEDLEY_UNLIKELY(0 > bind)) {
    upd_iso_msgf(iso, "tcp bind failure (%s:%"PRIu16")\n", host, port);
    upd_file_unref(f);
    return NULL;
  }

  const int listen = uv_listen(
    (uv_stream_t*) &srv->tcp, TCP_BACKLOG_, srv_conn_cb_);
  if (HEDLEY_UNLIKELY(0 > listen)) {
    upd_iso_msgf(iso, "tcp listen failure (%s:%"PRIu16")\n", host, port);
    upd_file_unref(f);
    return NULL;
  }
  upd_file_ref(f);
  srv->running = true;
  return f;
}


static bool srv_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  srv_t_* srv = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&srv, sizeof(*srv)))) {
    return false;
  }
  *srv = (srv_t_) {
    .tcp = { .data = f, },
    .watch = {
      .file  = f,
      .udata = f,
      .cb    = srv_watch_cb_,
    },
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&srv->watch))) {
    upd_free(&srv);
    return false;
  }
  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&iso->loop, &srv->tcp))) {
    upd_file_unwatch(&srv->watch);
    upd_free(&srv);
    return false;
  }
  f->ctx = srv;
  return true;
}

static void srv_deinit_(upd_file_t* f) {
  srv_t_* srv = f->ctx;

  upd_file_unwatch(&srv->watch);

  srv->tcp.data = srv;
  uv_close((uv_handle_t*) &srv->tcp, srv_close_cb_);
  upd_file_unref(srv->prog);
}

static bool srv_handle_(upd_req_t* req) {
  (void) req;
  return false;
}


static bool cli_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  cli_t_* cli = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&cli, sizeof(*cli)))) {
    return false;
  }
  *cli = (cli_t_) {
    .tcp      = { .data = f, },
    .shutdown = { .data = cli, },
    .watch    = {
      .file  = f,
      .udata = f,
      .cb    = cli_watch_cb_,
    },
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&cli->watch))) {
    upd_free(&cli);
    return false;
  }
  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&iso->loop, &cli->tcp))) {
    upd_file_unwatch(&cli->watch);
    upd_free(&cli);
    return false;
  }
  f->ctx = cli;
  return true;
}

static void cli_deinit_(upd_file_t* f) {
  cli_t_* cli = f->ctx;

  upd_file_unwatch(&cli->watch);

  cli->tcp.data = cli;
  const int shutdown = uv_shutdown(
    &cli->shutdown, (uv_stream_t*) &cli->tcp, cli_shutdown_cb_);
  if (HEDLEY_LIKELY(0 <= shutdown)) {
    return;
  }
  cli_shutdown_cb_(&cli->shutdown, 0);
}

static bool cli_handle_(upd_req_t* req) {
  (void) req;
  return false;
}

static void cli_close_(upd_file_t* f) {
  cli_t_* cli = f->ctx;

  upd_file_unwatch(&cli->watchst);
  uv_read_stop((uv_stream_t*) &cli->tcp);
  upd_file_unref(f);
}

static bool cli_pipe_stream_to_tcp_(upd_file_t* f) {
  cli_t_* cli = f->ctx;

  upd_file_ref(f);
  const bool read = upd_req_with_dup(&(upd_req_t) {
      .file = cli->k.file,
      .type = UPD_REQ_DSTREAM_READ,
      .stream = { .io = {
        .size = SIZE_MAX,
      }, },
      .udata = f,
      .cb    = cli_stream_read_cb_,
    });
  if (HEDLEY_UNLIKELY(!read)) {
    cli_close_(f);
    upd_file_unref(f);
    return false;
  }
  return true;
}


static void srv_conn_cb_(uv_stream_t* stream, int status) {
  upd_file_t* f   = stream->data;
  upd_iso_t*  iso = f->iso;
  srv_t_*     srv = f->ctx;

  if (HEDLEY_UNLIKELY(status < 0)) {
    upd_iso_msgf(iso,
      "tcp srv error: connection error (%s)\n", uv_err_name(status));
    return;
  }

  upd_file_t* fcli = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = &cli_,
    });
  if (HEDLEY_UNLIKELY(fcli == NULL)) {
    upd_iso_msgf(iso, "tcp srv error: client file creation failure\n");
    return;
  }
  cli_t_* cli = fcli->ctx;
  cli->srv = f;
  upd_file_ref(cli->srv);

  const int accept = uv_accept(
    (uv_stream_t*) &srv->tcp, (uv_stream_t*) &cli->tcp);
  if (HEDLEY_UNLIKELY(0 > accept)) {
    upd_file_unref(fcli);
    upd_iso_msgf(iso, "tcp srv error: accept failure\n");
    return;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = srv->prog,
      .udata = fcli,
      .cb    = cli_lock_prog_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unref(fcli);
    upd_iso_msgf(iso, "tcp srv error: program lock failure\n");
    return;
  }
}

static void srv_watch_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->udata;
  srv_t_*     srv = f->ctx;

  switch (w->event) {
  case UPD_FILE_SHUTDOWN:
    if (HEDLEY_LIKELY(srv->running)) {
      upd_file_unref(f);
    }
    break;
  }
}

static void srv_close_cb_(uv_handle_t* handle) {
  srv_t_* srv = handle->data;
  upd_free(&srv);
}


static void cli_lock_prog_cb_(upd_file_lock_t* k) {
  upd_file_t* f    = k->udata;
  upd_iso_t*  iso  = f->iso;
  upd_file_t* fpro = k->file;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_iso_msgf(iso, "tcp srv error: program lock cancelled\n");
    goto ABORT;
  }

  const bool exec = upd_req_with_dup(&(upd_req_t) {
      .file  = fpro,
      .type  = UPD_REQ_PROG_EXEC,
      .udata = k,
      .cb    = cli_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!exec)) {
    upd_iso_msgf(iso, "tcp cli error: program execution refused\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
  upd_file_unref(f);
}

static void cli_exec_cb_(upd_req_t* req) {
  upd_file_lock_t* kpro = req->udata;
  upd_file_t*      f    = kpro->udata;
  upd_iso_t*       iso  = f->iso;
  cli_t_*          cli  = f->ctx;

  upd_file_t* fst = req->result == UPD_REQ_OK? req->prog.exec: NULL;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(fst == NULL)) {
    upd_iso_msgf(iso, "tcp cli error: program execution failure\n");
    goto ABORT;
  }

  cli->k = (upd_file_lock_t) {
    .file  = fst,
    .ex    = true,
    .udata = kpro,
    .cb    = cli_lock_stream_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&cli->k))) {
    upd_iso_msgf(iso, "tcp cli error: stream lock failure");
  }
  return;

ABORT:
  upd_file_unlock(kpro);
  upd_iso_unstack(iso, kpro);
  upd_file_unref(f);
}

static void cli_lock_stream_cb_(upd_file_lock_t* k) {
  upd_file_lock_t* kpro = k->udata;
  upd_file_t*      f    = kpro->udata;
  upd_iso_t*       iso  = f->iso;
  cli_t_*          cli  = f->ctx;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_iso_msgf(iso, "tcp cli error: stream lock cancelled\n");
    goto EXIT;
  }

  cli->watchst = (upd_file_watch_t) {
    .file  = cli->k.file,
    .udata = f,
    .cb    = cli_watch_stream_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&cli->watchst))) {
    upd_file_unref(f);
    upd_iso_msgf(iso, "tcp cli error: stream watch failure\n");
    goto EXIT;
  }

  const int read_start = uv_read_start(
    (uv_stream_t*) &cli->tcp, cli_alloc_cb_, cli_tcp_read_cb_);
  if (HEDLEY_UNLIKELY(0 > read_start)) {
    upd_file_unwatch(&cli->watchst);
    upd_file_unref(f);
    upd_iso_msgf(iso, "tcp cli error: read_start failure");
    goto EXIT;
  }

  cli_pipe_stream_to_tcp_(f);

EXIT:
  upd_file_unlock(kpro);
  upd_iso_unstack(iso, kpro);
}

static void cli_alloc_cb_(uv_handle_t* handle, size_t n, uv_buf_t* buf) {
  (void) handle;

  *buf = (uv_buf_t) {0};

  uint8_t* ptr = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ptr, n))) {
    return;
  }
  *buf = uv_buf_init((char*) ptr, n);
}

static void cli_watch_cb_(upd_file_watch_t* w) {
  upd_file_t* f = w->udata;

  switch (w->event) {
  case UPD_FILE_SHUTDOWN:
    cli_close_(f);
    break;
  }
}

static void cli_watch_stream_cb_(upd_file_watch_t* w) {
  upd_file_t* f = w->udata;

  switch (w->event) {
  case UPD_FILE_UPDATE:
    cli_pipe_stream_to_tcp_(f);
    break;
  }
}

static void cli_tcp_read_cb_(uv_stream_t* stream, ssize_t n, const uv_buf_t* buf) {
  upd_file_t* f   = stream->data;
  cli_t_*     cli = f->ctx;

  uint8_t* ptr = (void*) buf->base;

  if (HEDLEY_UNLIKELY(n < 0)) {
    goto ABORT;
  }

  upd_file_ref(f);
  const bool write = upd_req_with_dup(&(upd_req_t) {
      .file = cli->k.file,
      .type = UPD_REQ_DSTREAM_WRITE,
      .stream = { .io = {
        .buf  = ptr,
        .size = n,
      }, },
      .udata = f,
      .cb    = cli_stream_write_cb_,
    });
  if (HEDLEY_UNLIKELY(!write)) {
    upd_file_unref(f);
    goto ABORT;
  }
  return;

ABORT:
  upd_free(&ptr);
  cli_close_(f);
}

static void cli_tcp_write_cb_(uv_write_t* req, int status) {
  upd_file_t* f   = req->data;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(0 > status)) {
    upd_iso_msgf(iso,
      "tcp cli error: tcp write failure (%s)\n", uv_err_name(status));
  }
  upd_iso_unstack(iso, req);
  upd_file_unref(f);
}

static void cli_stream_read_cb_(upd_req_t* req) {
  upd_file_t* f   = req->udata;
  cli_t_*     cli = f->ctx;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    goto EXIT;
  }

  const upd_req_stream_io_t* io = &req->stream.io;
  if (HEDLEY_UNLIKELY(io->size == 0)) {
    goto EXIT;
  }

  uv_write_t* w = upd_iso_stack(iso, sizeof(*w)+io->size);
  if (HEDLEY_UNLIKELY(w == NULL)) {
    upd_iso_unstack(iso, w);
    cli_close_(f);
    upd_iso_msgf(iso, "tcp cli error: tcp write req allocation failure\n");
    goto EXIT;
  }
  *w = (uv_write_t) { .data = f, };
  memcpy(w+1, io->buf, io->size);

  const uv_buf_t buf = uv_buf_init((char*) (w+1), io->size);

  upd_file_ref(f);
  const int write = uv_write(
    w, (uv_stream_t*) &cli->tcp, &buf, 1, cli_tcp_write_cb_);
  if (HEDLEY_UNLIKELY(0 > write)) {
    upd_file_unref(f);
    upd_iso_unstack(iso, w);
    cli_close_(f);
    upd_iso_msgf(iso,
      "tcp cli error: tcp write failure (%s)\n", uv_err_name(write));
    goto EXIT;
  }

EXIT:
  upd_iso_unstack(iso, req);
  upd_file_unref(f);
}

static void cli_stream_write_cb_(upd_req_t* req) {
  upd_file_t* f   = req->udata;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    cli_close_(f);
  }
  upd_free(&req->stream.io.buf);
  upd_iso_unstack(iso, req);
  upd_file_unref(f);
}

static void cli_shutdown_cb_(uv_shutdown_t* req, int status) {
  (void) status;

  cli_t_* cli = req->data;
  uv_close((uv_handle_t*) &cli->tcp, cli_close_cb_);

  upd_file_unref(cli->srv);
  if (HEDLEY_LIKELY(cli->k.file)) {
    upd_file_unlock(&cli->k);
  }
}

static void cli_close_cb_(uv_handle_t* handle) {
  cli_t_* cli = handle->data;
  upd_free(&cli);
}
