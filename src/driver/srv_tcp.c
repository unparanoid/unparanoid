#include "common.h"

#define LOG_PREFIX_ "upd.srv.tcp: "

#define TCP_BACKLOG_ 255


typedef struct srv_t_ {
  upd_file_t* prog;

  struct sockaddr_in addr;

  upd_file_t* tcp;
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
srv_parse_param_(
  upd_file_t* f);

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

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
srv_logf_(
  upd_file_t* f,
  const char* fmt,
  ...);

const upd_driver_t upd_driver_srv_tcp = {
  .name   = (uint8_t*) "upd.srv.tcp",
  .cats   = (upd_req_cat_t[]) {0},
  .init   = srv_init_,
  .deinit = srv_deinit_,
  .handle = srv_handle_,
};


static
bool
tcp_init_(
  upd_file_t* f);

static
void
tcp_deinit_(
  upd_file_t* f);

static
bool
tcp_handle_(
  upd_req_t* req);

static
void
tcp_close_(
  upd_file_t* f);

static const upd_driver_t tcp_ = {
  .name   = (uint8_t*) "upd.srv.tcp.internal_",
  .cats   = (upd_req_cat_t[]) {0},
  .init   = tcp_init_,
  .deinit = tcp_deinit_,
  .handle = tcp_handle_,
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

static
bool
cli_pipe_stream_to_tcp_(
  upd_file_t* f);

static const upd_driver_t cli_ = {
  .name   = (uint8_t*) "upd.srv.tcp.cli_",
  .cats   = (upd_req_cat_t[]) {0},
  .init   = cli_init_,
  .deinit = cli_deinit_,
  .handle = cli_handle_,
};


static
void
srv_pathfind_cb_(
  upd_pathfind_t* pf);

static
void
srv_conn_cb_(
  uv_stream_t* stream,
  int          status);


static
void
tcp_close_cb_(
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


static bool srv_parse_param_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  srv_t_*    srv = f->ctx;

  bool ret = false;

  yaml_document_t doc = {0};
  if (HEDLEY_UNLIKELY(!upd_yaml_parse(&doc, f->param, f->paramlen))) {
    return false;
  }

  uint64_t port = 0;
  const yaml_node_t* bind = NULL;
  const yaml_node_t* path = NULL;

  const char* invalid =
    upd_yaml_find_fields_from_root(&doc, (upd_yaml_field_t[]) {
        { .name = "port", .required = true,  .ui  = &port, },
        { .name = "bind", .required = false, .str = &bind, },
        { .name = "path", .required = true,  .str = &path, },
        { NULL, },
      });
  if (HEDLEY_UNLIKELY(invalid)) {
    upd_iso_msgf(iso, LOG_PREFIX_"invalid param field: %s\n", invalid);
    goto EXIT;
  }

  if (HEDLEY_UNLIKELY(port == 0 || UINT16_MAX < port)) {
    upd_iso_msgf(iso, LOG_PREFIX_"invalid port: %"PRIuMAX"\n", port);
    goto EXIT;
  }

  uint8_t bind_c[16] = "0.0.0.0";
  if (bind) {
    const uint8_t* b    = bind->data.scalar.value;
    const size_t   blen = bind->data.scalar.length;

    if (HEDLEY_UNLIKELY(blen >= sizeof(bind_c))) {
      upd_iso_msgf(iso, LOG_PREFIX_"too long bind address: %.*s\n", (int) blen, b);
      goto EXIT;
    }
    utf8ncpy(bind_c, b, blen);
    bind_c[blen] = 0;
  }

  if (HEDLEY_UNLIKELY(0 > uv_ip4_addr((char*) bind_c, port, &srv->addr))) {
    upd_iso_msgf(iso,
      LOG_PREFIX_"invalid bind address or port: %s:%"PRIuMAX"\n", bind_c, port);
    goto EXIT;
  }

  const bool pf = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso   = iso,
      .path  = path->data.scalar.value,
      .len   = path->data.scalar.length,
      .udata = f,
      .cb    = srv_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!pf)) {
    upd_iso_msgf(iso, LOG_PREFIX_"pathfind failure\n");
    goto EXIT;
  }

  ret = true;
EXIT:
  yaml_document_delete(&doc);
  return ret;
}

static bool srv_init_(upd_file_t* f) {
  srv_t_* srv = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&srv, sizeof(*srv)))) {
    return false;
  }
  *srv = (srv_t_) {0};
  f->ctx = srv;

  if (HEDLEY_UNLIKELY(!srv_parse_param_(f))) {
    upd_free(&srv);
    return false;
  }
  return true;
}

static void srv_deinit_(upd_file_t* f) {
  srv_t_* srv = f->ctx;

  if (HEDLEY_LIKELY(srv->prog)) {
    upd_file_unref(srv->prog);
  }
  if (HEDLEY_LIKELY(srv->tcp)) {
    tcp_close_(srv->tcp);
  }
  upd_free(&srv);
}

static bool srv_handle_(upd_req_t* req) {
  (void) req;
  return false;
}

static void srv_logf_(upd_file_t* f, const char* fmt, ...) {
  srv_t_* srv = f->ctx;

  char temp[256];

  va_list args;
  va_start(args, fmt);
  vsnprintf(temp, sizeof(temp), fmt, args);
  va_end(args);

  const uint16_t port = srv->addr.sin_port;

  union {
    uint32_t u32;
    uint8_t  u8[4];
  } ip = { .u32 = srv->addr.sin_addr.s_addr, };

  upd_iso_msgf(f->iso,
    LOG_PREFIX_"%s (%"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8":%"PRIu16")\n",
    temp, ip.u8[0], ip.u8[1], ip.u8[2], ip.u8[3],
    (port << 8 | port >> 8) & 0xFFFF);
}


static bool tcp_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  uv_tcp_t* tcp = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&tcp, sizeof(*tcp)))) {
    return false;
  }
  *tcp = (uv_tcp_t) {0};

  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&iso->loop, tcp))) {
    upd_free(&tcp);
    return false;
  }
  f->ctx = tcp;
  return true;
}

static void tcp_deinit_(upd_file_t* f) {
  upd_free(&f->ctx);
}

static void tcp_close_(upd_file_t* f) {
  uv_tcp_t* tcp = f->ctx;
  tcp->data = f;
  uv_close((uv_handle_t*) tcp, tcp_close_cb_);
}

static bool tcp_handle_(upd_req_t* req) {
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


static void srv_pathfind_cb_(upd_pathfind_t* pf) {
  upd_file_t* f   = pf->udata;
  upd_iso_t*  iso = f->iso;
  srv_t_*     srv = f->ctx;

  srv->prog = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(srv->prog == NULL)) {
    srv_logf_(f, "program pathfind failure");
    return;
  }
  upd_file_ref(srv->prog);

  upd_file_t* tcpf = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = &tcp_,
    });
  if (HEDLEY_UNLIKELY(tcpf == NULL)) {
    srv_logf_(f, "tcp allocation failure");
    return;
  }

  uv_tcp_t* tcp = tcpf->ctx;
  tcp->data = f;

  const int bind = uv_tcp_bind(tcp, (struct sockaddr*) &srv->addr, 0);
  if (HEDLEY_UNLIKELY(0 > bind)) {
    srv_logf_(f, "tcp bind error: %s", uv_err_name(bind));
    upd_file_unref(tcpf);
    return;
  }

  const int listen = uv_listen((uv_stream_t*) tcp, TCP_BACKLOG_, srv_conn_cb_);
  if (HEDLEY_UNLIKELY(0 > listen)) {
    srv_logf_(f, "tcp listen failure: %s", uv_err_name(listen));
    upd_file_unref(tcpf);
    return;
  }
  srv->tcp = tcpf;
}

static void srv_conn_cb_(uv_stream_t* stream, int status) {
  upd_file_t* f   = stream->data;
  upd_iso_t*  iso = f->iso;
  srv_t_*     srv = f->ctx;
  uv_tcp_t*   tcp = srv->tcp->ctx;

  if (HEDLEY_UNLIKELY(status < 0)) {
    srv_logf_(f, "tcp connection error: %s", uv_err_name(status));
    return;
  }

  upd_file_t* fcli = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = &cli_,
    });
  if (HEDLEY_UNLIKELY(fcli == NULL)) {
    srv_logf_(f, "tcp client allocation failure");
    return;
  }
  cli_t_* cli = fcli->ctx;
  cli->srv = f;
  upd_file_ref(cli->srv);

  const int accept = uv_accept((uv_stream_t*) tcp, (uv_stream_t*) &cli->tcp);
  if (HEDLEY_UNLIKELY(0 > accept)) {
    upd_file_unref(fcli);
    srv_logf_(f, "acception failure");
    return;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = srv->prog,
      .udata = fcli,
      .cb    = cli_lock_prog_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unref(fcli);
    srv_logf_(f, "program lock refusal");
    return;
  }
}


static void tcp_close_cb_(uv_handle_t* handle) {
  upd_file_t* f = handle->data;
  upd_file_unref(f);
}


static void cli_lock_prog_cb_(upd_file_lock_t* k) {
  upd_file_t* f    = k->udata;
  upd_iso_t*  iso  = f->iso;
  cli_t_*     cli  = f->ctx;
  upd_file_t* fpro = k->file;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    srv_logf_(cli->srv, "program lock cancelled");
    goto ABORT;
  }

  const bool exec = upd_req_with_dup(&(upd_req_t) {
      .file  = fpro,
      .type  = UPD_REQ_PROG_EXEC,
      .udata = k,
      .cb    = cli_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!exec)) {
    srv_logf_(cli->srv, "program execution refusal");
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
    srv_logf_(cli->srv, "program execution failure");
    goto ABORT;
  }

  cli->k = (upd_file_lock_t) {
    .file  = fst,
    .ex    = true,
    .udata = kpro,
    .cb    = cli_lock_stream_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&cli->k))) {
    srv_logf_(cli->srv, "stream lock refusal");
    goto ABORT;
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
    srv_logf_(cli->srv, "stream lock cancelled");
    goto EXIT;
  }

  cli->watchst = (upd_file_watch_t) {
    .file  = cli->k.file,
    .udata = f,
    .cb    = cli_watch_stream_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&cli->watchst))) {
    upd_file_unref(f);
    srv_logf_(cli->srv, "stream watch failure");
    goto EXIT;
  }

  const int read_start = uv_read_start(
    (uv_stream_t*) &cli->tcp, cli_alloc_cb_, cli_tcp_read_cb_);
  if (HEDLEY_UNLIKELY(0 > read_start)) {
    upd_file_unwatch(&cli->watchst);
    upd_file_unref(f);
    srv_logf_(cli->srv, "read_start failure: %s", uv_err_name(read_start));
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
  cli_t_*     cli = f->ctx;

  if (HEDLEY_UNLIKELY(0 > status)) {
    srv_logf_(cli->srv, "tcp write failure: %s", uv_err_name(status));
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
    srv_logf_(cli->srv, "tcp write req allocation failure");
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
    srv_logf_(cli->srv, "tcp write failure: %s", uv_err_name(write));
    goto EXIT;
  }

  if (HEDLEY_UNLIKELY(io->tail)) {
    cli_close_(f);
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
