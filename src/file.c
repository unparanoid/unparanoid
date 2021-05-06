#include "common.h"


#define FILE_POLL_INTERVAL_ 1500


static
void
file_poll_cb_(
  uv_fs_poll_t*    poll,
  int              status,
  const uv_stat_t* prev,
  const uv_stat_t* curr);

static
void
file_poll_close_cb_(
  uv_handle_t* handle);


upd_file_t* upd_file_new_from_npath(
    upd_iso_t*          iso,
    const upd_driver_t* driver,
    const uint8_t*      npath,
    size_t              len) {
  upd_file_t_* f = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&f, sizeof(*f)+len+!!len))) {
    return NULL;
  }
  *f = (upd_file_t_) {
    .super = {
      .iso    = iso,
      .driver = driver,
      .npath  = len? (uint8_t*) (f+1): NULL,
      .id     = iso->files_created++,
      .refcnt = 1,
    },
  };

  if (f->super.npath) {
    utf8ncpy(f->super.npath, npath, len);
    f->super.npath[len] = 0;

    if (HEDLEY_UNLIKELY(!upd_malloc(&f->poll, sizeof(*f->poll)))) {
      upd_free(&f);
      return NULL;
    }
    if (HEDLEY_UNLIKELY(!uv_fs_poll_init(&iso->loop, f->poll))) {
      upd_free(&f->poll);
      upd_free(&f);
      return NULL;
    }
    f->poll->data = f;

    const bool start = 0 <= uv_fs_poll_start(
      f->poll, file_poll_cb_, (char*) f->super.npath, FILE_POLL_INTERVAL_);
    if (HEDLEY_UNLIKELY(!start)) {
      uv_close((uv_handle_t*) f->poll, file_poll_close_cb_);
      upd_free(&f);
      return NULL;
    }
  }

  if (HEDLEY_UNLIKELY(!driver->init(&f->super))) {
    uv_close((uv_handle_t*) &f->poll, file_poll_close_cb_);
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

  if (f_->poll) {
    uv_fs_poll_stop(f_->poll);
    uv_close((uv_handle_t*) f_->poll, file_poll_close_cb_);
  }

  upd_file_trigger(f, UPD_FILE_DELETE);
  upd_array_clear(&f_->watch);

  upd_array_find_and_remove(&f->iso->files, f);
  f->driver->deinit(f);
  upd_free(&f_);
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

static void file_poll_close_cb_(uv_handle_t* handle) {
  upd_free(&handle);
}
