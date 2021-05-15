#include "common.h"


#define FILE_POLL_INTERVAL_ 1500


static
bool
file_normalize_npath_(
  upd_iso_t*     iso,
  uint8_t**      dst,
  size_t*        len,
  const uint8_t* src);


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
  uint8_t* np = NULL;
  if (HEDLEY_UNLIKELY(!file_normalize_npath_(iso, &np, &len, npath))) {
    return NULL;
  }
  upd_file_t* f = upd_file_new_from_normalized_npath(iso, driver, np, len);
  upd_iso_unstack(iso, np);
  return f;
}

upd_file_t* upd_file_new_from_normalized_npath(
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

  if (len) {
    utf8ncpy(f->super.npath, npath, len);
    f->super.npath[len] = 0;
    if (HEDLEY_UNLIKELY(!upd_malloc(&f->poll, sizeof(*f->poll)))) {
      upd_free(&f);
      return NULL;
    }

    if (HEDLEY_UNLIKELY(0 > uv_fs_poll_init(&iso->loop, f->poll))) {
      upd_free(&f->poll);
      upd_free(&f);
      return NULL;
    }
    f->poll->data = f;
    uv_unref((uv_handle_t*) f->poll);

    const bool start = 0 <= uv_fs_poll_start(
      f->poll, file_poll_cb_, (char*) f->super.npath, FILE_POLL_INTERVAL_);
    if (HEDLEY_UNLIKELY(!start)) {
      uv_close((uv_handle_t*) f->poll, file_poll_close_cb_);
      upd_free(&f);
      return NULL;
    }
  }

  if (HEDLEY_UNLIKELY(!driver->init(&f->super))) {
    if (len) {
      uv_close((uv_handle_t*) &f->poll, file_poll_close_cb_);
    }
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


static bool file_normalize_npath_(
    upd_iso_t* iso, uint8_t** dst, size_t* len, const uint8_t* src) {
  if (HEDLEY_UNLIKELY(*len == 0)) {
    *dst = NULL;
    return true;
  }

  uint8_t* s = upd_iso_stack(iso, *len+1);
  if (HEDLEY_UNLIKELY(s == NULL)) {
    return false;
  }
  utf8ncpy(s, src, *len);
  s[*len] = 0;

  const uint8_t* prefix = iso->path.working;
  if (HEDLEY_UNLIKELY(*len || src[0] == '|')) {
    prefix = iso->path.runtime;
    ++src;
  }

  *len = cwk_path_join((char*) prefix, (char*) s, NULL, 0);

  uint8_t* j = upd_iso_stack(iso, *len + 1);
  if (HEDLEY_UNLIKELY(j == NULL)) {
    upd_iso_unstack(iso, s);
    return false;
  }
  cwk_path_join((char*) prefix, (char*) s, (char*) j, *len + 1);
  upd_iso_unstack(iso, s);

  *len = cwk_path_normalize((char*) j, NULL, 0);

  uint8_t* n = upd_iso_stack(iso, *len + 1);
  if (HEDLEY_UNLIKELY(n == NULL)) {
    upd_iso_unstack(iso, j);
    return false;
  }
  cwk_path_normalize((char*) j, (char*) n, *len + 1);
  upd_iso_unstack(iso, j);

  *dst = n;
  return true;
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
