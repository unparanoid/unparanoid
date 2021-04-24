#pragma once

#include "common.h"


HEDLEY_NON_NULL(1, 2)
static inline upd_file_t* upd_file_new(
    upd_iso_t* iso, const upd_driver_t* driver) {
  upd_file_t* f = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&f, sizeof(*f)))) {
    return NULL;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->files, f, SIZE_MAX))) {
    upd_free(&f);
    return NULL;
  }

  *f = (upd_file_t) {
    .iso    = iso,
    .driver = driver,
    .id     = ++iso->files_created,
    .refcnt = 1,
  };
  driver->init(f);
  return f;
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
    if (f[i]->id < id) {
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
static inline void upd_file_unref(upd_file_t* f) {
  assert(f->refcnt);
  if (HEDLEY_UNLIKELY(!--f->refcnt)) {
    f->driver->deinit(f);
    upd_free(&f);
  }
}

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_file_watch(
    upd_file_t* f, uint64_t* id, upd_file_watch_cb_t cb, void* udata) {
  (void) f;
  (void) id;
  (void) cb;
  (void) udata;
  /* TODO */
  return true;
}

HEDLEY_NON_NULL(1)
static inline bool upd_file_unwatch(upd_file_t* f, uint64_t id) {
  (void) f;
  (void) id;
  /* TODO */
  return true;
}
