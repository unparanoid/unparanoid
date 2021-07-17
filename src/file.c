#include "common.h"


#define FILE_POLL_INTERVAL_ 1500


static
bool
file_init_poll_(
  upd_file_t_* f);

static
bool
file_init_prepare_(
  upd_file_t_* f);

static
bool
file_init_check_(
  upd_file_t_* f);

static
bool
file_init_async_(
  upd_file_t_* f);

static
bool
file_init_timer_(
  upd_file_t_* f);

static
void
file_close_all_handlers_(
  upd_file_t_* f);


static
void
file_poll_cb_(
  uv_fs_poll_t*    poll,
  int              status,
  const uv_stat_t* prev,
  const uv_stat_t* curr);

static
void
file_prepare_cb_(
  uv_prepare_t* handle);

static
void
file_check_cb_(
  uv_check_t* handle);

static
void
file_async_cb_(
  uv_async_t* f);

static
void
file_handle_close_cb_(
  uv_handle_t* handle);


upd_file_t* upd_file_new_(const upd_file_t* src) {
  upd_iso_t*          iso = src->iso;
  const upd_driver_t* d   = src->driver;

  const size_t pathlen  = src->pathlen  + !!src->pathlen;
  const size_t npathlen = src->npathlen + !!src->npathlen;
  const size_t paramlen = src->paramlen + !!src->paramlen;

  const size_t size = pathlen + npathlen + paramlen;

  upd_file_t_* f = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&f, sizeof(*f)+size))) {
    return NULL;
  }
  *f = (upd_file_t_) {
    .super = {
      .iso      = iso,
      .driver   = d,
      .id       = iso->files_created++,
      .refcnt   = 1,

      .last_update = 0,
      .last_req    = 0,
    },
  };

  size_t offset = 0;
# define assign_(N) do {  \
    if (N##len) {  \
      f->super.N       = (uint8_t*) (f+1) + offset;  \
      f->super.N##len  = N##len-1;  \
      offset          += N##len;  \
      utf8ncpy(f->super.N, src->N, N##len-1);  \
      f->super.N[N##len-1] = 0;  \
    }  \
  } while (0)

  assign_(path);
  assign_(npath);
  assign_(param);

# undef assign_

  const bool ok =
    (!d->flags.npoll || !npathlen || file_init_poll_(f))    &&
    (!d->flags.preproc            || file_init_prepare_(f)) &&
    (!d->flags.postproc           || file_init_check_(f))   &&
    (!d->flags.async              || file_init_async_(f))   &&
    (!d->flags.timer              || file_init_timer_(f))   &&
    d->init(&f->super);

  if (HEDLEY_UNLIKELY(!ok)) {
    file_close_all_handlers_(f);
    upd_free(&f);
    return NULL;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->files, f, SIZE_MAX))) {
    upd_file_unref(&f->super);
    return NULL;
  }
  return &f->super;
}

void upd_file_delete(upd_file_t* f) {
  assert(!f->refcnt);

  upd_file_t_* f_ = (void*) f;
  assert(!f_->lock.pending.n);
  assert(!f_->lock.refcnt);

  file_close_all_handlers_(f_);

  upd_file_trigger(f, UPD_FILE_DELETE);
  upd_array_clear(&f_->watch);

  upd_array_find_and_remove(&f->iso->files, f);
  f->driver->deinit(f);
  upd_free(&f_);
}


static bool file_init_poll_(upd_file_t_* f) {
  upd_iso_t* iso = f->super.iso;

  if (HEDLEY_UNLIKELY(!upd_malloc(&f->poll, sizeof(*f->poll)))) {
    return false;
  }

  *f->poll = (uv_fs_poll_t) { .data = f, };
  if (HEDLEY_UNLIKELY(0 > uv_fs_poll_init(&iso->loop, f->poll))) {
    upd_free(&f->poll);
    return false;
  }
  uv_unref((uv_handle_t*) f->poll);

  const bool start = 0 <= uv_fs_poll_start(
    f->poll, file_poll_cb_, (char*) f->super.npath, FILE_POLL_INTERVAL_);
  if (HEDLEY_UNLIKELY(!start)) {
    uv_close((uv_handle_t*) f->poll, file_handle_close_cb_);
    f->poll = NULL;
    return false;
  }
  return true;
}

static bool file_init_prepare_(upd_file_t_* f) {
  upd_iso_t* iso = f->super.iso;

  if (HEDLEY_UNLIKELY(!upd_malloc(&f->prepare, sizeof(*f->prepare)))) {
    return false;
  }

  *f->prepare = (uv_prepare_t) { .data = f, };
  if (HEDLEY_UNLIKELY(0 > uv_prepare_init(&iso->loop, f->prepare))) {
    upd_free(&f->prepare);
    return false;
  }
  uv_unref((uv_handle_t*) f->prepare);

  if (HEDLEY_UNLIKELY(0 > uv_prepare_start(f->prepare, file_prepare_cb_))) {
    uv_close((uv_handle_t*) f->prepare, file_handle_close_cb_);
    f->prepare = NULL;
    return false;
  }
  return true;
}

static bool file_init_check_(upd_file_t_* f) {
  upd_iso_t* iso = f->super.iso;

  if (HEDLEY_UNLIKELY(!upd_malloc(&f->check, sizeof(*f->check)))) {
    return false;
  }

  *f->check = (uv_check_t) { .data = f, };
  if (HEDLEY_UNLIKELY(0 > uv_check_init(&iso->loop, f->check))) {
    upd_free(&f->check);
    return false;
  }
  uv_unref((uv_handle_t*) f->check);

  if (HEDLEY_UNLIKELY(0 > uv_check_start(f->check, file_check_cb_))) {
    uv_close((uv_handle_t*) f->check, file_handle_close_cb_);
    f->check = NULL;
    return false;
  }
  return true;
}

static bool file_init_async_(upd_file_t_* f) {
  upd_iso_t* iso = f->super.iso;

  if (HEDLEY_UNLIKELY(!upd_malloc(&f->async, sizeof(*f->async)))) {
    return false;
  }
  *f->async = (uv_async_t) { .data = f, };

  const int err = uv_async_init(&iso->loop, f->async, file_async_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    upd_free(&f->async);
    return false;
  }
  uv_unref((uv_handle_t*) f->async);
  return true;
}

static bool file_init_timer_(upd_file_t_* f) {
  upd_iso_t* iso = f->super.iso;

  if (HEDLEY_UNLIKELY(!upd_malloc(&f->timer, sizeof(*f->timer)))) {
    return false;
  }
  *f->timer = (uv_timer_t) { .data = f, };

  const int err = uv_timer_init(&iso->loop, f->timer);
  if (HEDLEY_UNLIKELY(0 > err)) {
    upd_free(&f->timer);
    return false;
  }
  return true;
}

static void file_close_all_handlers_(upd_file_t_* f) {
  if (f->poll) {
    uv_fs_poll_stop(f->poll);
    uv_close((uv_handle_t*) f->poll, file_handle_close_cb_);
    f->poll = NULL;
  }
  if (HEDLEY_UNLIKELY(f->prepare)) {
    uv_prepare_stop(f->prepare);
    uv_close((uv_handle_t*) f->prepare, file_handle_close_cb_);
    f->prepare = NULL;
  }
  if (HEDLEY_UNLIKELY(f->check)) {
    uv_check_stop(f->check);
    uv_close((uv_handle_t*) f->check, file_handle_close_cb_);
    f->check = NULL;
  }
  if (f->async) {
    uv_close((uv_handle_t*) f->async, file_handle_close_cb_);
    f->async = NULL;
  }
  if (f->timer) {
    uv_close((uv_handle_t*) f->timer, file_handle_close_cb_);
    f->timer = NULL;
  }
}


static void file_poll_cb_(
    uv_fs_poll_t*    poll,
    int              status,
    const uv_stat_t* prev,
    const uv_stat_t* curr) {
  (void) prev;
  (void) curr;

  upd_file_t* f = poll->data;

  if (HEDLEY_UNLIKELY(status < 0)) {
    upd_file_trigger(f, UPD_FILE_DELETE_N);
    return;
  }
  upd_file_trigger(f, UPD_FILE_UPDATE_N);
}

static void file_prepare_cb_(uv_prepare_t* handle) {
  upd_file_t* f = handle->data;
  upd_file_trigger(f, UPD_FILE_PREPROC);
}

static void file_check_cb_(uv_check_t* handle) {
  upd_file_t* f = handle->data;
  upd_file_trigger(f, UPD_FILE_POSTPROC);
}

static void file_async_cb_(uv_async_t* handle) {
  upd_file_t* f = handle->data;
  upd_file_trigger(f, UPD_FILE_ASYNC);
}

static void file_handle_close_cb_(uv_handle_t* handle) {
  upd_free(&handle);
}
