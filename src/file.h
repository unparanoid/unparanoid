#pragma once

#include "common.h"


typedef struct upd_file_t_ {
  upd_file_t super;

  upd_array_of(upd_file_watch_t*) watch;

  uv_fs_poll_t* poll;
  uv_prepare_t* prepare;
  uv_check_t*   check;
  uv_async_t*   async;
  uv_timer_t*   timer;

  struct {
    size_t refcnt;
    bool   ex;
    upd_array_of(upd_file_lock_t*) pending;
  } lock;
} upd_file_t_;


HEDLEY_NON_NULL(1)
upd_file_t*
upd_file_new_(
  const upd_file_t* src);

HEDLEY_NON_NULL(1)
void
upd_file_delete(
  upd_file_t* f);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
bool
upd_file_try_lock(
  upd_file_lock_t* lock);


static inline upd_file_t* upd_file_new(const upd_file_t* src) {
  return upd_file_new_(src);
}

static inline upd_file_t* upd_file_get(upd_iso_t* iso, upd_file_id_t id) {
  upd_file_t** f = (upd_file_t**) iso->files.p;

  ssize_t l = 0;
  ssize_t r = (ssize_t) iso->files.n - 1;
  while (l <= r) {
    const size_t i = (l+r)/2;
    if (HEDLEY_UNLIKELY(f[i]->id == id)) {
      return f[i];
    }
    if (f[i]->id > id) {
      r = i-1;
    } else {
      l = i+1;
    }
  }
  return NULL;
}


static inline void upd_file_ref(upd_file_t* f) {
  ++f->refcnt;
}

static inline bool upd_file_unref(upd_file_t* f) {
  assert(f->refcnt);
  if (HEDLEY_UNLIKELY(!--f->refcnt)) {
    upd_file_delete(f);
    return true;
  }
  return false;
}


static inline bool upd_file_watch(upd_file_watch_t* w) {
  upd_file_t_* f = (void*) w->file;
  if (HEDLEY_UNLIKELY(!upd_array_insert(&f->watch, w, SIZE_MAX))) {
    return false;
  }
  return true;
}

static inline void upd_file_unwatch(upd_file_watch_t* w) {
  upd_file_t_* f = (void*) w->file;
  upd_array_find_and_remove(&f->watch, w);
}

static inline void upd_file_trigger(upd_file_t* f, upd_file_event_t e) {
  if (HEDLEY_UNLIKELY(e != UPD_FILE_DELETE)) {
    upd_file_ref(f);
  }
  upd_file_t_* f_ = (void*) f;
  for (size_t i = 0; i < f_->watch.n; ++i) {
    upd_file_watch_t* w = f_->watch.p[i];
    w->event = e;
    w->cb(w);
  }
  if (HEDLEY_UNLIKELY(e != UPD_FILE_DELETE)) {
    upd_file_unref(f);
  }
}

static inline bool upd_file_trigger_async(upd_file_t* f) {
  upd_file_t_* f_ = (void*) f;
  assert(f_->async);

  return f_->async && 0 <= uv_async_send(f_->async);
}

static inline void upd_file_trigger_timer_cb_(uv_timer_t* timer) {
  upd_file_t_* f_ = timer->data;
  upd_file_t*  f  = &f_->super;
  assert(f_->timer);

  if (HEDLEY_LIKELY(!upd_file_unref(f))) {
    upd_file_trigger(f, UPD_FILE_TIMER);
  }
}
static inline bool upd_file_trigger_timer(upd_file_t* f, uint64_t dur) {
  upd_file_t_* f_ = (void*) f;

  const int err = uv_timer_start(f_->timer, upd_file_trigger_timer_cb_, dur, 0);
  if (HEDLEY_UNLIKELY(0 > err)) {
    return false;
  }
  upd_file_ref(f);
  return true;
}


static inline bool upd_file_try_lock(upd_file_lock_t* l) {
  upd_file_t_* f = (void*) l->file;

  if (HEDLEY_UNLIKELY(f->lock.refcnt)) {
    if (HEDLEY_UNLIKELY(f->lock.ex || l->ex || f->lock.pending.n)) {
      return false;
    }
  }

  if (HEDLEY_LIKELY(f->lock.refcnt++ == 0)) {
    upd_file_ref(&f->super);  /* for locking */
  }

  f->lock.ex = l->ex;
  l->ok = true;

  /* be careful that the lock may be deleted in this callback */
  l->cb(l);
  return true;
}

static inline bool upd_file_lock(upd_file_lock_t* l) {
  upd_file_t_* f = (void*) l->file;

  if (HEDLEY_LIKELY(upd_file_try_lock(l))) {
    return true;
  }
  if (HEDLEY_UNLIKELY(l->timeout == 1)) {  /* trylock failure */
    return false;
  }
  l->ok = false;
  if (HEDLEY_UNLIKELY(!upd_array_insert(&f->lock.pending, l, SIZE_MAX))) {
    return false;
  }

  l->basetime = upd_iso_now(f->super.iso);
  if (HEDLEY_UNLIKELY(l->timeout == 0)) {
    l->timeout = UPD_FILE_LOCK_DEFAULT_TIMEOUT;
  }
  upd_file_ref(&f->super);  /* for queing */
  return true;
}

static inline void upd_file_unlock(upd_file_lock_t* l) {
  upd_file_t_* f = (void*) l->file;

  if (HEDLEY_LIKELY(upd_array_find_and_remove(&f->lock.pending, l))) {
    l->ok = false;
    l->cb(l);
    upd_file_unref(&f->super);  /* for dequeing */
    return;
  }
  if (HEDLEY_UNLIKELY(!l->ok)) {
    return;
  }
  l->ok = false;

  assert(f->lock.refcnt);
  if (HEDLEY_UNLIKELY(--f->lock.refcnt)) {
    return;
  }

  upd_array_t* pen = &f->lock.pending;

  upd_file_lock_t* k;
  while (k = upd_array_remove(pen, 0), k && upd_file_try_lock(k)) {
    upd_file_unref(&f->super);  /* for dequeing */
  }
  if (HEDLEY_UNLIKELY(k && !upd_array_insert(&f->lock.pending, k, 0))) {
    k->ok = false;
    k->cb(k);
  }
  upd_file_unref(&f->super);  /* for unlocking */
}
