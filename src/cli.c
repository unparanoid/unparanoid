#include "common.h"


#define CLI_BUFFER_MAX_ (1024*1024*8)


HEDLEY_PRINTF_FORMAT(2, 3)
static
void
cli_logf_(
  upd_cli_t*  cli,
  const char* fmt,
  ...);

static
void
cli_calc_hash_(
  upd_cli_t* cli);

static
void
cli_unref_(
  upd_cli_t* cli);

static
bool
cli_read_start_(
  upd_cli_t* cli);

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
    .iso    = srv->iso,
    .dir    = srv->dir,
    .prog   = srv->prog,
    .refcnt = 1,
  };
  cli_calc_hash_(cli);

  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&cli->iso->loop, &cli->uv.tcp))) {
    cli_logf_(cli, "tcp init failure");
    upd_free(&cli);
    return NULL;
  }

  upd_file_ref(cli->dir);
  upd_file_ref(cli->prog);

  if (HEDLEY_UNLIKELY(0 > uv_accept(&srv->uv.stream, &cli->uv.stream))) {
    cli_logf_(cli, "accept failure");
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
  if (HEDLEY_LIKELY(cli->deleted)) {
    return;
  }
  cli->deleted = true;
  cli_unref_(cli);
  upd_array_find_and_remove(&cli->iso->cli, cli);
}

static void cli_logf_(upd_cli_t* cli, const char* fmt, ...) {
  upd_iso_msgf(cli->iso, "cli error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(cli->iso, fmt, args);
  va_end(args);

  upd_iso_msgf(cli->iso, "\n");
}

static void cli_calc_hash_(upd_cli_t* cli) {
  static const uint8_t realm[] = "kawaii neko";

  const uint64_t now = upd_iso_now(cli->iso);

  SHA1_CTX sha1;
  sha1_init(&sha1);
  sha1_update(&sha1, (uint8_t*) &cli, sizeof(cli));
  sha1_update(&sha1, (uint8_t*) &now, sizeof(now));
  sha1_update(&sha1, realm, sizeof(realm)-1);

  uint8_t temp[SHA1_BLOCK_SIZE];
  sha1_final(&sha1, temp);

  base64_encode(temp, cli->hash, SHA1_BLOCK_SIZE, false);

  /* replaces '/' -> '-' because filename cannot include '/' */
  for (size_t i = 0; i < UPD_CLI_HASH_SIZE; ++i) {
    if (HEDLEY_UNLIKELY(cli->hash[i] == '/')) {
      cli->hash[i] = '-';
    }
  }
}

static void cli_unref_(upd_cli_t* cli) {
  assert(cli->refcnt);
  if (HEDLEY_UNLIKELY(--cli->refcnt)) {
    return;
  }

  upd_file_unref(cli->dir);
  upd_file_unref(cli->prog);
  if (HEDLEY_LIKELY(cli->io)) {
    upd_file_unwatch(&cli->watch);
    upd_file_unref(cli->io);
  }

  uv_shutdown_t* req = upd_iso_stack(cli->iso, sizeof(*req));
  if (HEDLEY_UNLIKELY(req == NULL)) {
    cli_logf_(cli, "shutdown req allocation failure");
    uv_close(&cli->uv.handle, cli_close_cb_);
    return;
  }

  *req = (uv_shutdown_t) { .data = cli, };
  if (HEDLEY_UNLIKELY(0 > uv_shutdown(req, &cli->uv.stream, cli_shutdown_cb_))) {
    cli_logf_(cli, "shutdown failure");
    upd_iso_unstack(cli->iso, req);
    uv_close(&cli->uv.handle, cli_close_cb_);
    return;
  }
}

static bool cli_read_start_(upd_cli_t* cli) {
  return 0 <= uv_read_start(&cli->uv.stream, cli_alloc_cb_, cli_read_cb_);
}

static bool cli_try_parse_(upd_cli_t* cli) {
  if (HEDLEY_LIKELY(cli->buf.parsing || !cli->buf.size)) {
    return true;
  }
  if (HEDLEY_UNLIKELY(0 > uv_read_stop(&cli->uv.stream))) {
    cli_logf_(cli, "read stop failure");
    return false;
  }

  cli->buf.parsing = cli->buf.size;

  ++cli->refcnt;
  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = cli->io,
      .ex    = true,
      .udata = cli,
      .cb    = cli_lock_for_input_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    cli_logf_(cli, "stream file lock failure");
    cli_unref_(cli);
    return false;
  }
  return true;
}


static void cli_lock_for_exec_cb_(upd_file_lock_t* l) {
  upd_cli_t* cli = l->udata;

  if (HEDLEY_UNLIKELY(!l->ok)) {
    cli_logf_(cli, "program lock failure");
    goto ABORT;
  }

  const bool exec = upd_req_with_dup(&(upd_req_t) {
      .file  = cli->prog,
      .type  = UPD_REQ_PROG_EXEC,
      .udata = l,
      .cb    = cli_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!exec)) {
    cli_logf_(cli, "exec req refused");
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

  cli->io = req->prog.exec;
  upd_iso_unstack(iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(cli->iso, lock);

  if (HEDLEY_UNLIKELY(cli->io == NULL)) {
    cli_logf_(cli, "exec req failure");
    goto ABORT;
  }

  cli->watch = (upd_file_watch_t) {
    .file  = cli->io,
    .udata = cli,
    .cb    = cli_watch_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&cli->watch))) {
    cli_logf_(cli, "watch failure");
    goto ABORT;
  }

  const bool ok = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = cli->dir,
      .ex    = true,
      .udata = cli,
      .cb    = cli_lock_for_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    cli_logf_(cli, "dir lock failure");
    goto ABORT;
  }
  return;

ABORT:
  upd_cli_delete(cli);
}

static void cli_lock_for_add_cb_(upd_file_lock_t* l) {
  upd_cli_t* cli = l->udata;

  if (HEDLEY_UNLIKELY(!l->ok)) {
    cli_logf_(cli, "dir lock failure");
    goto ABORT;
  }

  upd_req_t* req = upd_iso_stack(cli->iso, sizeof(*req));
  if (HEDLEY_UNLIKELY(req == NULL)) {
    cli_logf_(cli, "dir add req allocation failure");
    goto ABORT;
  }
  *req = (upd_req_t) {
    .file = cli->dir,
    .type = UPD_REQ_DIR_ADD,
    .dir  = { .entry = {
      .file    = cli->io,
      .name    = cli->hash,
      .len     = sizeof(cli->hash),
      .weakref = true,
    }, },
    .udata = l,
    .cb    = cli_add_cb_,
  };

  if (HEDLEY_UNLIKELY(!upd_req(req))) {
    upd_iso_unstack(cli->iso, req);
    cli_logf_(cli, "dir add req refused");
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
    cli_logf_(cli, "dir add req failure");
    goto ABORT;
  }

  const int err = uv_read_start(&cli->uv.stream, cli_alloc_cb_, cli_read_cb_);
  if (HEDLEY_UNLIKELY(err < 0)) {
    cli_logf_(cli, "read_start failure");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&cli->iso->cli, cli, SIZE_MAX))) {
    cli_logf_(cli, "insertion failure");
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
    cli_logf_(cli, "buffer allocation failure");
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
    cli_logf_(cli, "parse failure");
    upd_cli_delete(cli);
    return;
  }

  if (HEDLEY_UNLIKELY(!cli_read_start_(cli))) {
    cli_logf_(cli, "failed to resume reading");
    upd_cli_delete(cli);
    return;
  }
}

static void cli_lock_for_input_cb_(upd_file_lock_t* lock) {
  upd_cli_t* cli = lock->udata;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    cli_logf_(cli, "stream file lock failure");
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
    cli_logf_(cli, "stream file refused input req");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(cli->iso, lock);

  cli_unref_(cli);
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
      cli_logf_(cli, "continuous parse failure");
      upd_cli_delete(cli);
    }
  }
  cli_unref_(cli);
}

static void cli_watch_cb_(upd_file_watch_t* w) {
  upd_cli_t* cli = w->udata;

  if (HEDLEY_LIKELY(w->event == UPD_FILE_UPDATE)) {
    ++cli->refcnt;
    const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = cli->io,
        .ex    = true,
        .udata = cli,
        .cb    = cli_lock_for_output_cb_,
      });
    if (HEDLEY_UNLIKELY(!lock)) {
      cli_logf_(cli, "failed to lock stream file to output");
      cli_unref_(cli);
      upd_cli_delete(cli);
      return;
    }
  }
}

static void cli_lock_for_output_cb_(upd_file_lock_t* lock) {
  upd_cli_t* cli = lock->udata;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    cli_logf_(cli, "failed to lock stream file to output");
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

  cli_unref_(cli);
  upd_cli_delete(cli);
}

static void cli_output_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_cli_t*       cli  = lock->udata;
  upd_iso_t*       iso  = cli->iso;

  const upd_req_stream_io_t io = req->stream.io;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!io.size)) {
    upd_file_unlock(lock);
    upd_iso_unstack(iso, lock);
    cli_unref_(cli);
    return;
  }

  uv_write_t* write = upd_iso_stack(iso, sizeof(*write)+io.size);
  if (HEDLEY_UNLIKELY(write == NULL)) {
    cli_logf_(cli, "write req allocation failure");
    goto ABORT;
  }
  memcpy(write+1, io.buf, io.size);

  const uv_buf_t buf = uv_buf_init((char*) (write+1), io.size);

  *write = (uv_write_t) { .data = cli, };
  const bool ok = 0 <= uv_write(write, &cli->uv.stream, &buf, 1, cli_write_cb_);
  if (HEDLEY_UNLIKELY(!ok)) {
    cli_logf_(cli, "write failure");
    goto ABORT;
  }

  upd_file_unlock(lock);
  upd_iso_unstack(cli->iso, lock);
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
  cli_unref_(cli);
  upd_cli_delete(cli);
}

static void cli_write_cb_(uv_write_t* req, int status) {
  upd_cli_t* cli = req->data;
  upd_iso_unstack(cli->iso, req);

  if (HEDLEY_UNLIKELY(status < 0)) {
    cli_logf_(cli, "write failure");
    upd_cli_delete(cli);
  }
  cli_unref_(cli);
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
