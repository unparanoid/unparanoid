#include "common.h"


#define CLI_BUFFER_MAX_ (1024*1024*8)


static
bool
cli_try_parse_(
  upd_cli_t* cli);


static
void
cli_alloc_cb_(
  uv_handle_t* handle,
  size_t       n,
  uv_buf_t*    buf);

static
void
cli_read_cb_(
  uv_stream_t*    stream,
  ssize_t         n,
  const uv_buf_t* buf);

static
void
cli_parse_cb_(
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


static bool cli_accept_and_read_start_(upd_cli_t* cli, upd_srv_t* srv) {
  return
    0 <= uv_accept(&srv->uv.stream, &cli->uv.stream) &&
    0 <= uv_read_start(&cli->uv.stream, cli_alloc_cb_, cli_read_cb_);
}

upd_cli_t* upd_cli_new_tcp(upd_srv_t* srv) {
  upd_cli_t* cli = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&cli, sizeof(*cli)))) {
    return NULL;
  }
  *cli = (upd_cli_t) {
    .iso = srv->iso,
  };

  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&cli->iso->loop, &cli->uv.tcp))) {
    upd_free(&cli);
    return NULL;
  }
  if (HEDLEY_UNLIKELY(!cli_accept_and_read_start_(cli, srv))) {
    upd_cli_delete(cli);
    return NULL;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&cli->iso->cli, cli, SIZE_MAX))) {
    upd_cli_delete(cli);
    return NULL;
  }
  return cli;
}

void upd_cli_delete(upd_cli_t* cli) {
  upd_array_find_and_remove(&cli->iso->cli, cli);

  uv_shutdown_t* req = upd_iso_stack(cli->iso, sizeof(*req));
  if (HEDLEY_UNLIKELY(req == NULL)) {
    uv_close(&cli->uv.handle, cli_close_cb_);
    return;
  }

  *req = (uv_shutdown_t) { .data = cli, };
  if (HEDLEY_UNLIKELY(0 > uv_shutdown(req, &cli->uv.stream, cli_shutdown_cb_))) {
    upd_iso_unstack(cli->iso, req);
    uv_close(&cli->uv.handle, cli_close_cb_);
    return;
  }
}


static bool cli_try_parse_(upd_cli_t* cli) {
  if (HEDLEY_LIKELY(cli->parsing || !cli->buf.size)) {
    return true;
  }
  cli->parsing = true;

  return upd_req_with_dup(&(upd_req_t) {
      .file = cli->inout,
      .type = UPD_REQ_STREAM_INPUT,
      .stream = { .io = {
        .size = cli->buf.size,
        .buf  = cli->buf.ptr,
      }, },
      .udata = cli,
      .cb    = cli_parse_cb_,
    });
}


static void cli_alloc_cb_(uv_handle_t* handle, size_t n, uv_buf_t* buf) {
  upd_cli_t* cli = (void*) handle;

  size_t newsz = cli->buf.size + n;
  if (HEDLEY_UNLIKELY(newsz > CLI_BUFFER_MAX_)) {
    newsz = CLI_BUFFER_MAX_;
    if (HEDLEY_UNLIKELY(cli->buf.size <= CLI_BUFFER_MAX_)) {
      n = CLI_BUFFER_MAX_ - cli->buf.size;
    }
  }

  if (HEDLEY_UNLIKELY(!upd_malloc(&cli->buf.ptr, newsz))) {
    *buf = (uv_buf_t) {0};
    return;
  }
  *buf = uv_buf_init((char*) (cli->buf.ptr+cli->buf.size), n);
}

static void cli_read_cb_(uv_stream_t* stream, ssize_t n, const uv_buf_t* buf) {
  (void) buf;

  upd_cli_t* cli = (void*) stream;

  if (HEDLEY_UNLIKELY(n < 0)) {
    upd_cli_delete(cli);
    return;
  }
  cli->buf.size += n;
  if (HEDLEY_UNLIKELY(!cli_try_parse_(cli))) {
    upd_cli_delete(cli);
    return;
  }
}

static void cli_parse_cb_(upd_req_t* req) {
  upd_iso_t* iso = req->file->iso;
  upd_cli_t* cli = req->udata;

  const size_t consumed = req->stream.io.size;
  upd_iso_unstack(iso, req);

  assert(cli->buf.size >= consumed);
  cli->buf.size -= consumed;
  memmove(cli->buf.ptr, cli->buf.ptr+consumed, cli->buf.size);

  cli_try_parse_(cli);
}

static void cli_shutdown_cb_(uv_shutdown_t* req, int status) {
  (void) status;

  upd_cli_t* cli = req->data;
  upd_iso_unstack(cli->iso, req);
  upd_free(&cli->buf.ptr);
  uv_close(&cli->uv.handle, cli_close_cb_);
}

static void cli_close_cb_(uv_handle_t* handle) {
  upd_cli_t* cli = (void*) handle;
  upd_free(&cli);
}
