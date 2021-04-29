#include "common.h"


#define CLI_BUFFER_MAX_ (1024*1024*8)


static
bool
cli_try_parse_(
  upd_cli_t* cli);


static
void
cli_exec_cb_(
  upd_req_t* req);

static
void
cli_lock_cb_(
  upd_file_lock_t* l);

static
void
cli_add_cb_(
  upd_req_t* req);

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
cli_rm_cb_(
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


upd_cli_t* upd_cli_new_tcp(upd_srv_t* srv) {
  upd_cli_t* cli = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&cli, sizeof(*cli)))) {
    return NULL;
  }
  *cli = (upd_cli_t) {
    .iso = srv->iso,
    .dir = srv->dir,
  };

  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&cli->iso->loop, &cli->uv.tcp))) {
    upd_free(&cli);
    return NULL;
  }

  upd_file_ref(cli->dir);

  if (HEDLEY_UNLIKELY(0 > uv_accept(&srv->uv.stream, &cli->uv.stream))) {
    upd_cli_delete(cli);
    return NULL;
  }
  const bool exec = upd_req_with_dup(&(upd_req_t) {
      .file  = srv->prog,
      .type  = UPD_REQ_PROGRAM_EXEC,
      .udata = cli,
      .cb    = cli_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!exec)) {
    upd_cli_delete(cli);
    return NULL;
  }
  return cli;
}

void upd_cli_delete(upd_cli_t* cli) {
  upd_array_find_and_remove(&cli->iso->cli, cli);

  const bool rm = upd_req_with_dup(&(upd_req_t) {
      .file  = cli->dir,
      .type  = UPD_REQ_DIR_RM,
      .dir   = { .entry = {
        .file = cli->inout,
      }, },
      .udata = cli,
      .cb    = cli_rm_cb_,
    });
  (void) rm;

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


static void cli_exec_cb_(upd_req_t* req) {
  upd_iso_t* iso = req->file->iso;
  upd_cli_t* cli = req->udata;

  cli->inout = req->program.exec;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(cli->inout == NULL)) {
    goto ABORT;
  }
  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = cli->dir,
      .ex    = true,
      .udata = cli,
      .cb    = cli_lock_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_cli_delete(cli);
}

static void cli_lock_cb_(upd_file_lock_t* l) {
  upd_cli_t* cli = l->udata;
  upd_iso_unstack(cli->iso, l);

  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file = cli->dir,
      .type = UPD_REQ_DIR_ADD,
      .dir  = { .entry = {
        .file = cli->inout,
        .name = (uint8_t*) "test",
        .len  = 4,
      }, },
      .udata = cli,
      .cb    = cli_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!add)) {
    upd_cli_delete(cli);
    return;
  }
}

static void cli_add_cb_(upd_req_t* req) {
  upd_cli_t* cli = req->udata;
  upd_iso_unstack(cli->iso, req);

  const int err = uv_read_start(&cli->uv.stream, cli_alloc_cb_, cli_read_cb_);
  if (HEDLEY_UNLIKELY(err < 0)) {
    upd_cli_delete(cli);
    return;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&cli->iso->cli, cli, SIZE_MAX))) {
    upd_cli_delete(cli);
    return;
  }
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
  cli->parsing = false;

  assert(cli->buf.size >= consumed);
  cli->buf.size -= consumed;
  memmove(cli->buf.ptr, cli->buf.ptr+consumed, cli->buf.size);

  cli_try_parse_(cli);
}

static void cli_rm_cb_(upd_req_t* req) {
  upd_iso_unstack(req->file->iso, req);
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

  upd_file_unref(cli->dir);
  if (HEDLEY_LIKELY(cli->inout)) {
    upd_file_unref(cli->inout);
  }
  upd_free(&cli);
}
