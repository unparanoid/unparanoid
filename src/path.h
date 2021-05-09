#pragma once

#include "common.h"


#define UPD_PATH_MAX 512


static inline size_t upd_path_normalize(uint8_t* path, size_t len) {
  if (HEDLEY_UNLIKELY(len == 0)) {
    return 0;
  }

  const uint8_t* in  = path;
  uint8_t*       out = path;

  *out = *(in++);
  for (size_t i = 1; i < len; ++i, ++in) {
    if (HEDLEY_LIKELY(*out != '/' || *in != '/')) {
      *(++out) = *in;
    }
  }
  return out - path + 1;
}

static inline bool upd_path_validate_name(const uint8_t* name, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (HEDLEY_UNLIKELY(name[i] == '/')) {
      return false;
    }
  }
  return true;
}

static inline size_t upd_path_drop_trailing_slash(
    const uint8_t* path, size_t len) {
  while (len && path[len-1] == '/') --len;
  return len;
}

static inline size_t upd_path_dirname(const uint8_t* path, size_t len) {
  len = upd_path_drop_trailing_slash(path, len);
  while (len && path[len-1] != '/') --len;
  return len;
}


#if defined(UPD_TEST)
static inline void upd_test_path(void) {
# define streq_(p, l, v) (utf8ncmp(p, v, l) == 0 && v[l] == 0)

  uint8_t      p1[] = "///hell//world//////";
  const size_t l1   = upd_path_normalize(p1, sizeof(p1)-1);
  assert(streq_(p1, l1, "/hell/world/"));

  assert( upd_path_validate_name((uint8_t*) "foo",     3));
  assert(!upd_path_validate_name((uint8_t*) "foo/baz", 7));

  uint8_t p2[] = "///hoge//piyo//////////////";
  assert(upd_path_drop_trailing_slash(
    p2, sizeof(p2)-1) == utf8size_lazy("///hoge//piyo"));

  assert(upd_path_dirname(p2, sizeof(p2)-1) == utf8size_lazy("///hoge//"));

# undef streq_
}
#endif
