#pragma once

#include "common.h"


typedef struct upd_file_t_ {
  upd_file_t super;

  upd_array_of(upd_file_watch_t*) watch;

  struct {
    size_t refcnt;
    bool   ex;
    upd_array_of(upd_file_lock_t*) pending;
  } lock;
} upd_file_t_;


HEDLEY_NON_NULL(1, 2)
static inline upd_file_t* upd_file_new(
    upd_iso_t* iso, const upd_driver_t* driver) {
  upd_file_t_* f = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&f, sizeof(*f)))) {
    return NULL;
  }

  *f = (upd_file_t_) {
    .super = {
      .iso    = iso,
      .driver = driver,
      .id     = iso->files_created++,
      .refcnt = 1,
    },
  };
  if (HEDLEY_UNLIKELY(!driver->init(&f->super))) {
    upd_free(&f);
    return NULL;
  }

  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->files, f, SIZE_MAX))) {
    upd_free(&f);
    return NULL;
  }
  return &f->super;
}

HEDLEY_NON_NULL(1)
static inline upd_file_t* upd_file_new_from_driver_name(
    upd_iso_t* iso, const uint8_t* name, size_t len) {
  const upd_driver_t* d = upd_driver_lookup(iso, name, len);
  return d? upd_file_new(iso, d): NULL;
}

HEDLEY_NON_NULL(1)
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


HEDLEY_NON_NULL(1)
static inline void upd_file_ref(upd_file_t* f) {
  ++f->refcnt;
}

HEDLEY_NON_NULL(1)
static inline bool upd_file_unref(upd_file_t* f) {
  assert(f->refcnt);
  if (HEDLEY_UNLIKELY(!--f->refcnt)) {
    upd_file_t_* f_ = (void*) f;

    upd_array_find_and_remove(&f->iso->files, f);
    f->driver->deinit(f);

    upd_file_trigger(f, UPD_FILE_DELETE);
    upd_array_clear(&f_->watch);

    assert(!f_->lock.pending.n);

    upd_free(&f_);
    return true;
  }
  return false;
}


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_file_watch(upd_file_watch_t* w) {
  upd_file_t_* f = (void*) w->file;
  if (HEDLEY_UNLIKELY(!upd_array_insert(&f->watch, w, SIZE_MAX))) {
    return false;
  }
  return true;
}

HEDLEY_NON_NULL(1)
static inline void upd_file_unwatch(upd_file_watch_t* w) {
  upd_file_t_* f = (void*) w->file;
  upd_array_find_and_remove(&f->watch, w);
}

HEDLEY_NON_NULL(1)
static inline void upd_file_trigger(upd_file_t* f, upd_file_event_t e) {
  upd_file_t_* f_ = (void*) f;
  for (size_t i = 0; i < f_->watch.n; ++i) {
    upd_file_watch_t* w = f_->watch.p[i];
    w->event = e;
    w->cb(w);
  }
}


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
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

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_file_lock(upd_file_lock_t* l) {
  upd_file_t_* f = (void*) l->file;

  if (HEDLEY_LIKELY(upd_file_try_lock(l))) {
    return true;
  }
  l->ok = false;
  if (HEDLEY_UNLIKELY(!upd_array_insert(&f->lock.pending, l, SIZE_MAX))) {
    return false;
  }
  upd_file_ref(&f->super);  /* for queing */
  return true;
}

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline upd_file_lock_t* upd_file_lock_with_dup(
    const upd_file_lock_t* l) {
  upd_file_lock_t* k = upd_iso_stack(l->file->iso, sizeof(*k));
  if (HEDLEY_UNLIKELY(k == NULL)) {
    return NULL;
  }
  *k = *l;
  if (HEDLEY_UNLIKELY(!upd_file_lock(k))) {
    upd_iso_unstack(l->file->iso, k);
    return NULL;
  }
  return k;
}

HEDLEY_NON_NULL(1)
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

  assert(f->lock.refcnt);
  if (HEDLEY_UNLIKELY(--f->lock.refcnt)) {
    return;
  }

  upd_file_unref(&f->super);  /* for unlocking */

  upd_array_t* pen = &f->lock.pending;

  upd_file_lock_t* k;
  while (k = upd_array_remove(pen, 0), k && upd_file_try_lock(k)) {
    upd_file_unref(&f->super);  /* for dequeing */
  }
  if (HEDLEY_UNLIKELY(k && !upd_array_insert(&f->lock.pending, k, 0))) {
    k->ok = false;
    k->cb(k);
  }
}


#if defined(UPD_TEST)
static void upd_test_file_watch_cb_(upd_file_watch_t* w) {
  bool* expect = w->udata;
  assert(*expect);
  *expect = false;
}
static void upd_test_file_lock_cb_(upd_file_lock_t* l) {
  bool* expect = l->udata;
  assert(*expect);
  *expect = false;
}
static void upd_test_file(void) {
  const uint8_t* dname = (void*) "upd.test.driver.null";

  upd_file_t* f = upd_file_new_from_driver_name(
    upd_test.iso, dname, utf8size_lazy(dname));
  assert(f);


  bool wexpect = false;
  upd_file_watch_t w = {
    .file  = f,
    .cb    = upd_test_file_watch_cb_,
    .udata = &wexpect,
  };
  assert(upd_file_watch(&w));

  wexpect = true;
  upd_file_trigger(f, UPD_FILE_UPDATE);
  assert(!wexpect);


  bool             expects[32] = { false, };
  upd_file_lock_t* locks[32]   = { NULL,  };

# define lock_(N)  \
  locks[N] = &(upd_file_lock_t) {  \
      .file  = f,  \
      .udata = &expects[N],  \
      .cb    = upd_test_file_lock_cb_,  \
    };  \
  assert(upd_file_lock(locks[N]));
# define exlock_(N)  \
  locks[N] = &(upd_file_lock_t) {  \
      .file  = f,  \
      .ex    = true,  \
      .udata = &expects[N],  \
      .cb    = upd_test_file_lock_cb_,  \
    };  \
  assert(upd_file_lock(locks[N]));

  expects[0] = true;
  lock_(0);
  assert(!expects[0]);

  expects[1] = true;
  lock_(1);
  assert(!expects[1]);

  exlock_(2);
  exlock_(3);

  lock_(4);
  lock_(5);
  lock_(6);
  lock_(7);

  upd_file_unlock(locks[1]);

  expects[2] = true;
  upd_file_unlock(locks[0]);
  assert(!expects[2]);

  expects[4] = true;  /* cancel */
  upd_file_unlock(locks[4]);
  assert(!expects[4]);

  expects[3] = true;
  upd_file_unlock(locks[2]);
  assert(!expects[3]);

  expects[6] = true;  /* cancel */
  upd_file_unlock(locks[6]);
  assert(!expects[6]);

  expects[5] = true;
  expects[7] = true;
  upd_file_unlock(locks[3]);
  assert(!expects[5]);
  assert(!expects[7]);

  upd_file_unlock(locks[5]);
  upd_file_unlock(locks[7]);

# undef exlock_
# undef lock_

  upd_file_unwatch(&w);
  upd_file_unref(f);
}
#endif
