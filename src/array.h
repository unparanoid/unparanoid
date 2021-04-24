#pragma once

#include "common.h"


typedef struct upd_array_t {
  size_t n;
  void** p;
} upd_array_t;

#define upd_array_of(T) upd_array_t


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_array_resize(upd_array_t* a, size_t n) {
  size_t i = a->n;
  if (HEDLEY_UNLIKELY(!upd_malloc(&a->p, n*sizeof(*a->p)))) {
    return false;
  }
  for (; i < n; ++i) {
    a->p[i] = NULL;
  }
  a->n = n;
  return true;
}

HEDLEY_NON_NULL(1)
static inline void upd_array_clear(upd_array_t* a) {
  const bool ret = upd_array_resize(a, 0);
  (void) ret;
}

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_array_insert(upd_array_t* a, void* p, size_t i) {
  if (i > a->n) {
    i = a->n;
  }
  if (HEDLEY_UNLIKELY(!upd_array_resize(a, a->n+1))) {
    return false;
  }
  memmove(a->p+i+1, a->p+i, a->n-i-1);
  a->p[i] = p;
  return true;
}

HEDLEY_NON_NULL(1)
static inline void* upd_array_remove(upd_array_t* a, size_t i) {
  if (a->n == 0) {
    return NULL;
  }
  if (i >= a->n) {
    i = a->n-1;
  }

  void* ptr = a->p[i];
  memmove(a->p+i, a->p+i+1, a->n-i-1);

  const bool ret = upd_array_resize(a, a->n-1);
  (void) ret;

  return ptr;
}

HEDLEY_NON_NULL(1, 2)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_array_find(const upd_array_t* a, size_t* i, void* p) {
  for (size_t j = 0; j < a->n; ++j) {
    if (HEDLEY_UNLIKELY(a->p[j] == p)) {
      *i = j;
      return true;
    }
  }
  return false;
}


#if defined(UPD_TEST)
static void upd_test_array(void) {
  upd_array_t a = {0};

  assert(upd_array_insert(&a, (void*) 0x00, SIZE_MAX));
  assert(upd_array_insert(&a, (void*) 0x02, SIZE_MAX));
  assert(upd_array_insert(&a, (void*) 0x01, 1));

  size_t i;
  assert(upd_array_find(&a, &i, (void*) 0x00) && i == 0);
  assert(upd_array_find(&a, &i, (void*) 0x01) && i == 1);
  assert(upd_array_find(&a, &i, (void*) 0x02) && i == 2);
  assert(!upd_array_find(&a, &i, (void*) 0x03));

  assert(upd_array_remove(&a, 1) == (void*) 0x01);
  assert(!upd_array_find(&a, &i, (void*) 0x01));

  assert(upd_array_resize(&a, 3));
  assert(a.p[2] == NULL);

  upd_array_clear(&a);
  assert(a.n == 0);
  assert(a.p == NULL);
}
#endif
