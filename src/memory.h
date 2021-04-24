#pragma once

#include "common.h"


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_malloc(void* p, size_t n) {
  void** ptr = p;
  if (!*ptr) {
    if (n) {
      *ptr = malloc(n);
      return *ptr;
    }
    return true;
  }

  if (!n) {
    free(*ptr);
    *ptr = NULL;
    return true;
  }

  void* newptr = realloc(*ptr, n);
  if (HEDLEY_UNLIKELY(newptr == NULL)) {
    return false;
  }
  *ptr = newptr;
  return true;
}

HEDLEY_NON_NULL(1)
static inline void upd_free(void* p) {
  const bool ret = upd_malloc(p, 0);
  (void) ret;
}


#if !defined(NDEBUG)
static inline void upd_test_memory(void) {
  void* ptr = NULL;
  assert(upd_malloc(&ptr, 16));
  assert(upd_malloc(&ptr, 32));
  assert(upd_malloc(&ptr, 0));
  upd_free(&ptr);
}
#endif
