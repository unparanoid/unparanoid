#include "common.h"


#define LOG_PREFIX_ "upd.bin: "

#define BUF_MAX_ (1024*1024*8)  /* = 8 MiB */

#define DEFAULT_MIMETYPE_ "application/octet-stream"


typedef struct bin_t_  bin_t_;
typedef struct task_t_ task_t_;


struct bin_t_ {
  uv_file          fd;
  upd_file_watch_t watch;

  size_t bytes;

  task_t_* last_task;

  unsigned read  : 1;
  unsigned write : 1;
  unsigned open  : 1;
};

struct task_t_ {
  uv_fs_t fsreq;

  upd_file_t* file;
  upd_req_t*  req;
  task_t_*    next;

  uint8_t* buf;

  void
  (*exec)(
    task_t_* task);
};


static
void
bin_parse_param_(
  upd_file_t* f);

static
bool
bin_init_(
  upd_file_t* f);

static
void
bin_deinit_(
  upd_file_t* f);

static
bool
bin_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_bin = {
  .name = (uint8_t*) "upd.bin",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_STREAM,
    0,
  },
  .flags = {
    .npoll = true,
  },
  .init   = bin_init_,
  .deinit = bin_deinit_,
  .handle = bin_handle_,
};


static
bool
task_queue_with_dup_(
  const task_t_* task);

static
bool
task_queue_open_(
  upd_file_t* f);

static
void
task_finalize_(
  task_t_* task);


static
void
bin_watch_cb_(
  upd_file_watch_t* watch);

static
void
bin_deinit_close_cb_(
  uv_fs_t* fsreq);


static
void
task_stat_exec_cb_(
  task_t_* task);

static
void
task_stat_cb_(
  uv_fs_t* fsreq);

static
void
task_open_exec_cb_(
  task_t_* task);

static
void
task_open_cb_(
  uv_fs_t* fsreq);

static
void
task_read_exec_cb_(
  task_t_* task);

static
void
task_read_cb_(
  uv_fs_t* fsreq);

static
void
task_write_exec_cb_(
  task_t_* task);

static
void
task_write_cb_(
  uv_fs_t* fsreq);

static
void
task_truncate_exec_cb_(
  task_t_* task);

static
void
task_truncate_cb_(
  uv_fs_t* fsreq);

static
void
task_close_exec_cb_(
  task_t_* task);

static
void
task_close_cb_(
  uv_fs_t* fsreq);


static void bin_parse_param_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  bin_t_*    ctx = f->ctx;

  if (HEDLEY_UNLIKELY(f->paramlen == 0)) {
    return;
  }

  yaml_document_t doc = {0};
  if (HEDLEY_UNLIKELY(!upd_yaml_parse(&doc, f->param, f->paramlen))) {
    upd_iso_msgf(iso, LOG_PREFIX_"invalid param is ignored\n");
    return;
  }

  const yaml_node_t* mode = NULL;

  const char* invalid =
    upd_yaml_find_fields_from_root(&doc, (upd_yaml_field_t[]) {
        { .name = "mode", .str = &mode, },
        { NULL, },
      });
  if (HEDLEY_UNLIKELY(invalid)) {
    upd_iso_msgf(iso, LOG_PREFIX_"invalid param field '%s'\n", invalid);
    goto EXIT;
  }

  if (mode) {
    const uint8_t* mode_s = mode->data.scalar.value;
    const size_t   mode_n = mode->data.scalar.length;
    if (upd_strcaseq_c("rw", mode_s, mode_n)) {
      ctx->read  = true;
      ctx->write = true;

    } else if (upd_strcaseq_c("r", mode_s, mode_n)) {
      ctx->read  = true;
      ctx->write = false;

    } else if (upd_strcaseq_c("w", mode_s, mode_n)) {
      ctx->read  = false;
      ctx->write = true;

    } else {
      upd_iso_msgf(iso,
        LOG_PREFIX_"invalid mode: '%.*s'\n", (int) mode_n, mode_s);
      goto EXIT;
    }
  }

EXIT:
  yaml_document_delete(&doc);
}

static bool bin_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  if (HEDLEY_UNLIKELY(!f->npath)) {
    upd_iso_msgf(iso, LOG_PREFIX_"empty npath\n");
    return false;
  }

  {
    const char* ext;
    size_t      extlen;
    if (cwk_path_get_extension((char*) f->npath, &ext, &extlen)) {
      ++ext; --extlen;
      f->mimetype = (uint8_t*) mimetype_from_ext_n(ext, extlen);
    }
    if (HEDLEY_UNLIKELY(f->mimetype == NULL)) {
      f->mimetype = (uint8_t*) DEFAULT_MIMETYPE_;
    }
  }

  bin_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    upd_iso_msgf(iso, LOG_PREFIX_"context allocation failure\n");
    return false;
  }
  *ctx = (bin_t_) {
    .watch = {
      .file = f,
      .cb   = bin_watch_cb_,
    },
    .read  = true,
    .write = true,
  };
  f->ctx = ctx;
  bin_parse_param_(f);

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_iso_msgf(iso, LOG_PREFIX_"self watch failure\n");
    upd_free(&ctx);
    return false;
  }

  const bool q = task_queue_with_dup_(&(task_t_) {
      .file = f,
      .exec = task_stat_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!q)) {
    upd_iso_msgf(iso, LOG_PREFIX_"failed to queue first task\n");
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    return false;
  }
  return true;
}

static void bin_deinit_(upd_file_t* f) {
  bin_t_*    ctx = f->ctx;
  upd_iso_t* iso = f->iso;

  upd_file_unwatch(&ctx->watch);

  if (HEDLEY_LIKELY(!ctx->open)) {
    goto EXIT;
  }
  uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
  if (HEDLEY_UNLIKELY(fsreq == NULL)) {
    goto EXIT;
  }
  *fsreq = (uv_fs_t) { .data = iso, };

  const int err = uv_fs_close(&iso->loop, fsreq, ctx->fd, bin_deinit_close_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto EXIT;
  }

EXIT:
  upd_free(&ctx);
}

static bool bin_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  bin_t_*     ctx = f->ctx;

  switch (req->type) {
  case UPD_REQ_STREAM_READ: {
    if (HEDLEY_UNLIKELY(!ctx->read)) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    if (HEDLEY_UNLIKELY(!ctx->open && !task_queue_open_(f))) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    const bool ok = task_queue_with_dup_(&(task_t_) {
        .file = f,
        .req  = req,
        .exec = task_read_exec_cb_,
      });
    if (HEDLEY_UNLIKELY(!ok)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
  } return true;

  case UPD_REQ_STREAM_WRITE: {
    if (HEDLEY_UNLIKELY(!ctx->write)) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    if (HEDLEY_UNLIKELY(!ctx->open && !task_queue_open_(f))) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    const bool ok = task_queue_with_dup_(&(task_t_) {
        .file = f,
        .req  = req,
        .exec = task_write_exec_cb_,
      });
    if (HEDLEY_UNLIKELY(!ok)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
  } return true;

  case UPD_REQ_STREAM_TRUNCATE: {
    if (HEDLEY_UNLIKELY(!ctx->write)) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    if (HEDLEY_UNLIKELY(!ctx->open && !task_queue_open_(f))) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    const bool ok = task_queue_with_dup_(&(task_t_) {
        .file = f,
        .req  = req,
        .exec = task_truncate_exec_cb_,
      });
    if (HEDLEY_UNLIKELY(!ok)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}


static bool task_queue_with_dup_(const task_t_* src) {
  upd_file_t* f   = src->file;
  bin_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  task_t_* task = upd_iso_stack(iso, sizeof(*task));
  if (HEDLEY_UNLIKELY(task == NULL)) {
    return false;
  }

  upd_file_ref(f);
  *task = *src;
  if (HEDLEY_LIKELY(ctx->last_task)) {
    ctx->last_task->next = task;
    ctx->last_task       = task;
  } else {
    ctx->last_task = task;
    task->exec(task);
  }
  return task;
}

static bool task_queue_open_(upd_file_t* f) {
  return task_queue_with_dup_(&(task_t_) {
      .file = f,
      .exec = task_open_exec_cb_,
    });
}

static void task_finalize_(task_t_* task) {
  upd_file_t* f   = task->file;
  bin_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(ctx->last_task == task)) {
    ctx->last_task = NULL;
  } else if (HEDLEY_UNLIKELY(task->next)) {
    task->next->exec(task->next);
  }
  upd_iso_unstack(iso, task);
  upd_file_unref(f);
}


static void bin_watch_cb_(upd_file_watch_t* watch) {
  upd_file_t* f   = watch->file;
  bin_t_*     ctx = f->ctx;

  switch (watch->event) {
  case UPD_FILE_UPDATE_N:
    task_queue_with_dup_(&(task_t_) {
        .file = f,
        .exec = task_stat_exec_cb_,
      });
    if (HEDLEY_LIKELY(ctx->open)) {
      task_queue_with_dup_(&(task_t_) {
          .file = f,
          .exec = task_close_exec_cb_,
        });
      task_queue_with_dup_(&(task_t_) {
          .file = f,
          .exec = task_open_exec_cb_,
        });
    }
    upd_file_trigger(f, UPD_FILE_UPDATE);
    break;

  case UPD_FILE_UNCACHE:
    if (HEDLEY_UNLIKELY(ctx->open)) {
      task_queue_with_dup_(&(task_t_) {
          .file = f,
          .exec = task_close_exec_cb_,
        });
    }
    break;
  }
}

static void bin_deinit_close_cb_(uv_fs_t* fsreq) {
  upd_iso_t* iso = fsreq->data;
  upd_iso_unstack(iso, fsreq);
}


static void task_stat_exec_cb_(task_t_* task) {
  upd_file_t* f   = task->file;
  upd_iso_t*  iso = f->iso;

  const int err = uv_fs_stat(
    &iso->loop, &task->fsreq, (char*) f->npath, task_stat_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto ABORT;
  }
  return;

ABORT:
  task_finalize_(task);
}

static void task_stat_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_file_t* f    = task->file;
  bin_t_*     ctx  = f->ctx;

  const ssize_t result = fsreq->result;
  const size_t  bytes  = fsreq->statbuf.st_size;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    goto EXIT;
  }

  ctx->bytes = bytes;

EXIT:
  task_finalize_(task);
}

static void task_open_exec_cb_(task_t_* task) {
  upd_file_t* f   = task->file;
  bin_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  const int flag =
    ctx->read && ctx->write? O_RDWR:
    ctx->read?               O_RDONLY:
    ctx->write?              O_WRONLY: 0;

  const int open = uv_fs_open(
    &iso->loop, &task->fsreq, (char*) f->npath, flag, 0, task_open_cb_);
  if (HEDLEY_UNLIKELY(0 > open)) {
    goto ABORT;
  }
  return;

ABORT:
  task_finalize_(task);
}

static void task_open_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_file_t* f    = task->file;
  bin_t_*     ctx  = f->ctx;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    goto EXIT;
  }

  ctx->fd   = result;
  ctx->open = true;
  f->cache  = BUF_MAX_ < ctx->bytes? BUF_MAX_: ctx->bytes;

EXIT:
  task_finalize_(task);
}

static void task_read_exec_cb_(task_t_* task) {
  upd_file_t* f    = task->file;
  upd_req_t*  req  = task->req;
  bin_t_*     ctx  = f->ctx;
  upd_iso_t*  iso  = f->iso;

  if (HEDLEY_UNLIKELY(!ctx->open)) {
    req->result = UPD_REQ_ABORTED;
    goto ABORT;
  }

  const size_t off = req->stream.io.offset;

  size_t sz = req->stream.io.size;
  if (HEDLEY_LIKELY(sz+off > ctx->bytes)) {
    sz = ctx->bytes > off? ctx->bytes-off: 0;
  }
  if (HEDLEY_LIKELY(sz > BUF_MAX_)) {
    sz = BUF_MAX_;
  }
  if (HEDLEY_UNLIKELY(sz == 0)) {
    req->result = UPD_REQ_OK;
    goto ABORT;
  }

  task->buf = upd_iso_stack(iso, sz);
  if (HEDLEY_UNLIKELY(task->buf == NULL)) {
    req->result = UPD_REQ_NOMEM;
    goto ABORT;
  }

  const uv_buf_t buf = uv_buf_init((char*) task->buf, sz);

  const int err = uv_fs_read(
    &iso->loop, &task->fsreq, ctx->fd, &buf, 1, off, task_read_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    upd_iso_unstack(iso, task->buf);
    req->result = UPD_REQ_ABORTED;
    goto ABORT;
  }
  return;

ABORT:
  req->stream.io.size = 0;
  req->cb(req);
  task_finalize_(task);
}

static void task_read_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_file_t* f    = task->file;
  upd_req_t*  req  = task->req;
  upd_iso_t*  iso  = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    req->stream.io.size = 0;
    req->result = UPD_REQ_ABORTED;
    goto EXIT;
  }

  const size_t off = req->stream.io.offset;
  req->stream.io = (upd_req_stream_io_t) {
    .offset = off,
    .size   = result,
    .buf    = task->buf,
  };
  req->result = UPD_REQ_OK;

EXIT:
  req->cb(req);
  upd_iso_unstack(iso, task->buf);
  task_finalize_(task);
}

static void task_write_exec_cb_(task_t_* task) {
  upd_file_t* f    = task->file;
  upd_req_t*  req  = task->req;
  bin_t_*     ctx  = f->ctx;
  upd_iso_t*  iso  = f->iso;

  if (HEDLEY_UNLIKELY(!ctx->open)) {
    goto ABORT;
  }

  const size_t sz  = req->stream.io.size;
  const size_t off = req->stream.io.offset;

  const uv_buf_t buf = uv_buf_init((char*) req->stream.io.buf, sz);

  const int err = uv_fs_write(
    &iso->loop, &task->fsreq, ctx->fd, &buf, 1, off, task_write_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto ABORT;
  }
  return;

ABORT:
  req->stream.io.size = 0;
  req->result = UPD_REQ_ABORTED;
  req->cb(req);
  task_finalize_(task);
}

static void task_write_cb_(uv_fs_t* fsreq) {
  task_t_*   task = (void*) fsreq;
  upd_req_t* req  = task->req;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    req->stream.io.size = 0;
    req->result = UPD_REQ_ABORTED;
    goto EXIT;
  }
  req->result = UPD_REQ_OK;
  req->stream.io.size = result;

EXIT:
  req->cb(req);
  task_finalize_(task);
}

static void task_truncate_exec_cb_(task_t_* task) {
  upd_file_t* f   = task->file;
  upd_req_t*  req = task->req;
  bin_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!ctx->open)) {
    goto ABORT;
  }

  const size_t size = req->stream.io.size;

  const int err = uv_fs_ftruncate(
    &iso->loop, &task->fsreq, ctx->fd, size, task_truncate_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto ABORT;
  }
  return;

ABORT:
  req->result = UPD_REQ_ABORTED;
  req->cb(req);
  task_finalize_(task);
}

static void task_truncate_cb_(uv_fs_t* fsreq) {
  task_t_*   task = (void*) fsreq;
  upd_req_t* req  = task->req;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    req->result = UPD_REQ_ABORTED;
    goto EXIT;
  }
  req->result = UPD_REQ_OK;

EXIT:
  req->cb(req);
  task_finalize_(task);
}

static void task_close_exec_cb_(task_t_* task) {
  upd_file_t* f    = task->file;
  bin_t_*     ctx  = f->ctx;
  upd_iso_t*  iso  = f->iso;

  if (HEDLEY_UNLIKELY(!ctx->open)) {
    goto ABORT;
  }

  /* we don't care about if the file is actually closed */
  f->cache = 0;

  const int err = uv_fs_close(
    &iso->loop, &task->fsreq, ctx->fd, task_close_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto ABORT;
  }
  return;

ABORT:
  task_finalize_(task);
}

static void task_close_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_file_t* f    = task->file;
  bin_t_*     ctx  = f->ctx;

  ctx->open = false;
  task_finalize_(task);
}
