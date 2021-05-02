#include "common.h"


#define CLI_BUFFER_MAX_ (1024*1024*8)


static
bool
cli_try_parse_(
  upd_cli_t* cli);


static
void
cli_lock_for_exec_cb_(
  upd_file_lock_t* l);

static
void
cli_exec_cb_(
  upd_req_t* req);

static
void
cli_lock_for_add_cb_(
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
cli_lock_for_input_cb_(
  upd_file_lock_t* lock);

static
void
cli_input_cb_(
  upd_req_t* req);

static
void
cli_watch_cb_(
  upd_file_watch_t* w);

static
void
cli_lock_for_output_cb_(
  upd_file_lock_t* lock);

static
void
cli_output_cb_(
  upd_req_t* req);

static
void
cli_write_cb_(
  uv_write_t* req,
  int         status);

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
    .iso  = srv->iso,
    .dir  = srv->dir,
    .prog = srv->prog,
  };

  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&cli->iso->loop, &cli->uv.tcp))) {
    upd_free(&cli);
    return NULL;
  }

  upd_file_ref(cli->dir);
  upd_file_ref(cli->prog);

  if (HEDLEY_UNLIKELY(0 > uv_accept(&srv->uv.stream, &cli->uv.stream))) {
    upd_cli_delete(cli);
    return NULL;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = cli->dir,
      .udata = cli,
      .cb    = cli_lock_for_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
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
  if (HEDLEY_LIKELY(cli->buf.parsing || !cli->buf.size)) {
    return true;
  }
  cli->buf.parsing = cli->buf.size;

  return upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = cli->io,
      .ex    = true,
      .udata = cli,
      .cb    = cli_lock_for_input_cb_,
    });
}


static void cli_lock_for_exec_cb_(upd_file_lock_t* l) {
  upd_cli_t* cli = l->udata;

  if (HEDLEY_UNLIKELY(!l->ok)) {
    goto ABORT;
  }

  const bool exec = upd_req_with_dup(&(upd_req_t) {
      .file  = cli->prog,
      .type  = UPD_REQ_PROGRAM_EXEC,
      .udata = l,
      .cb    = cli_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!exec)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(l);
  upd_iso_unstack(cli->iso, l);
  upd_cli_delete(cli);
}

static void cli_exec_cb_(upd_req_t* req) {
  upd_iso_t*       iso  = req->file->iso;
  upd_file_lock_t* lock = req->udata;
  upd_cli_t*       cli  = lock->udata;

  cli->io = req->program.exec;
  upd_iso_unstack(iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(cli->iso, lock);

  if (HEDLEY_UNLIKELY(cli->io == NULL)) {
    goto ABORT;
  }

  cli->watch = (upd_file_watch_t) {
    .file  = cli->io,
    .udata = cli,
    .cb    = cli_watch_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&cli->watch))) {
    goto ABORT;
  }

  const bool ok = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = cli->dir,
      .ex    = true,
      .udata = cli,
      .cb    = cli_lock_for_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_cli_delete(cli);
}

static void cli_lock_for_add_cb_(upd_file_lock_t* l) {
  upd_cli_t* cli = l->udata;

  if (HEDLEY_UNLIKELY(!l->ok)) {
    goto ABORT;
  }

  uint8_t name[32];
  const size_t namelen = snprintf(
    (char*) name, sizeof(name), "%"PRIuPTR, (uintptr_t) cli);

  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file = cli->dir,
      .type = UPD_REQ_DIR_ADD,
      .dir  = { .entry = {
        .file    = cli->io,
        .name    = name,
        .len     = namelen,
        .weakref = true,
      }, },
      .udata = l,
      .cb    = cli_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!add)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(l);
  upd_iso_unstack(cli->iso, l);
  upd_cli_delete(cli);
}

static void cli_add_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_cli_t*       cli  = lock->udata;

  const bool ok = req->dir.entry.file;
  upd_iso_unstack(cli->iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(cli->iso, lock);

  if (HEDLEY_UNLIKELY(!ok)) {
    goto ABORT;
  }

  const int err = uv_read_start(&cli->uv.stream, cli_alloc_cb_, cli_read_cb_);
  if (HEDLEY_UNLIKELY(err < 0)) {
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&cli->iso->cli, cli, SIZE_MAX))) {
    goto ABORT;
  }
  return;

ABORT:
  upd_cli_delete(cli);
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

static void cli_lock_for_input_cb_(upd_file_lock_t* lock) {
  upd_cli_t* cli = lock->udata;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }

  const bool input = upd_req_with_dup(&(upd_req_t) {
      .file = cli->io,
      .type = UPD_REQ_STREAM_INPUT,
      .stream = { .io = {
        .size = cli->buf.size,
        .buf  = cli->buf.ptr,
      }, },
      .udata = lock,
      .cb    = cli_input_cb_,
    });
  if (HEDLEY_UNLIKELY(!input)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(cli->iso, lock);
  upd_cli_delete(cli);
}

static void cli_input_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_iso_t*       iso  = req->file->iso;
  upd_cli_t*       cli  = lock->udata;

  const size_t consumed = req->stream.io.size;

  upd_iso_unstack(iso, req);

  const bool retry = cli->buf.parsing != cli->buf.size;
  cli->buf.parsing = 0;

  assert(cli->buf.size >= consumed);
  cli->buf.size -= consumed;
  memmove(cli->buf.ptr, cli->buf.ptr+consumed, cli->buf.size);

  upd_file_unlock(lock);
  upd_iso_unstack(cli->iso, lock);

  if (HEDLEY_UNLIKELY(retry)) {
    if (HEDLEY_UNLIKELY(!cli_try_parse_(cli))) {
      upd_cli_delete(cli);
      return;
    }
  }
}

static void cli_watch_cb_(upd_file_watch_t* w) {
  upd_cli_t* cli = w->udata;

  if (HEDLEY_LIKELY(w->event == UPD_FILE_UPDATE)) {
    const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = cli->io,
        .ex    = true,
        .udata = cli,
        .cb    = cli_lock_for_output_cb_,
      });
    if (HEDLEY_UNLIKELY(!lock)) {
      upd_cli_delete(cli);
      return;
    }
  }
}

static void cli_lock_for_output_cb_(upd_file_lock_t* lock) {
  upd_cli_t* cli = lock->udata;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }

  const bool output = upd_req_with_dup(&(upd_req_t) {
      .file  = cli->io,
      .type  = UPD_REQ_STREAM_OUTPUT,
      .udata = lock,
      .cb    = cli_output_cb_,
    });
  if (HEDLEY_UNLIKELY(!output)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(cli->iso, lock);
  upd_cli_delete(cli);
}

static void cli_output_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_cli_t*       cli  = lock->udata;
  upd_iso_t*       iso  = cli->iso;

  const upd_req_stream_io_t* io = &req->stream.io;

  uv_write_t* write = upd_iso_stack(iso, sizeof(*write)+io->size);
  if (HEDLEY_UNLIKELY(write == NULL)) {
    upd_cli_delete(cli);
    goto EXIT;
  }
  memcpy(write+1, io->buf, io->size);

  const uv_buf_t buf = uv_buf_init((char*) (write+1), io->size);

  *write = (uv_write_t) { .data = cli, };
  const bool ok = 0 <= uv_write(write, &cli->uv.stream, &buf, 1, cli_write_cb_);
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_cli_delete(cli);
    goto EXIT;
  }

EXIT:
  upd_iso_unstack(iso, req);
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
}

static void cli_write_cb_(uv_write_t* req, int status) {
  upd_cli_t* cli = req->data;

  upd_iso_unstack(cli->iso, req);

  if (HEDLEY_UNLIKELY(status < 0)) {
    upd_cli_delete(cli);
  }
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
  upd_file_unref(cli->prog);
  if (HEDLEY_LIKELY(cli->io)) {
    upd_file_unref(cli->io);
  }
  upd_free(&cli);
}
