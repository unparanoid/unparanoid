#pragma once

#include "common.h"


typedef struct upd_map_t upd_map_t;

struct upd_map_t {
  uint8_t* s;
  void*    p;

  upd_map_t* l;
  upd_map_t* r;
};

#define upd_map_of(T) upd_map_t*


static inline void upd_map_clear(upd_map_t** map) {
  if (HEDLEY_UNLIKELY(*map == NULL)) return;

  upd_free(&(*map)->l);
  upd_free(&(*map)->r);
  upd_free(map);
}

static inline void upd_map_set_(upd_map_t** map, upd_map_t* m) {
  if (HEDLEY_UNLIKELY(*map == NULL)) {
    *map = m;
    return;
  }
  const int x = utf8cmp((*map)->s, m->s);
  if (HEDLEY_UNLIKELY(x == 0)) {
    (*map)->p = m->p;
    upd_free(&m);
    return;
  }
  upd_map_set_(x < 0? &(*map)->l: &(*map)->r, m);
}

static inline bool upd_map_set(
    upd_map_t** map, const uint8_t* s, size_t len, void* p) {
  upd_map_t* m = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&m, sizeof(*m)+len+1))) {
    return false;
  }
  *m = (upd_map_t) {
    .s = (uint8_t*) (m+1),
    .p = p,
  };
  utf8ncpy(m->s, s, len);
  m->s[len] = 0;

  upd_map_set_(map, m);
  return true;
}

static inline upd_map_t** upd_map_get_(
    upd_map_t** map, const uint8_t* s, size_t len) {
  if (HEDLEY_UNLIKELY(*map == NULL)) {
    return NULL;
  }
  const int x = utf8ncmp((*map)->s, s, len);
  if (HEDLEY_UNLIKELY(x == 0)) {
    return map;
  }
  return upd_map_get_(x < 0? &(*map)->l: &(*map)->r, s, len);
}

static inline void* upd_map_get(upd_map_t** map, const uint8_t* s, size_t len) {
  upd_map_t** m = upd_map_get_(map, s, len);
  if (HEDLEY_UNLIKELY(m == NULL)) {
    return NULL;
  }
  return m? (*m)->p: NULL;
}


#if defined(UPD_TEST)
static inline void upd_test_map(void) {
  upd_map_of(void*) map = {0};

  assert(upd_map_set(&map, (uint8_t*) "hello",   5, (void*) 0x01));
  assert(upd_map_set(&map, (uint8_t*) "world",   5, (void*) 0x02));
  assert(upd_map_set(&map, (uint8_t*) "hello",   5, (void*) 0x03));
  assert(upd_map_set(&map, (uint8_t*) "goodbye", 7, (void*) 0x04));

  assert(upd_map_get(&map, (uint8_t*) "hello",   5) == (void*) 0x03);
  assert(upd_map_get(&map, (uint8_t*) "world",   5) == (void*) 0x02);
  assert(upd_map_get(&map, (uint8_t*) "goodbye", 7) == (void*) 0x04);

  assert(upd_map_get(&map, (uint8_t*) "hoge", 4) == NULL);

  upd_map_clear(&map);
  assert(map == NULL);

  assert(upd_map_set(&map, (uint8_t*) "hoge", 4, (void*) 0x01));
  assert(upd_map_set(&map, (uint8_t*) "fuga", 4, (void*) 0x02));
  assert(upd_map_set(&map, (uint8_t*) "piyo", 4, (void*) 0x03));
  assert(upd_map_set(&map, (uint8_t*) "poyo", 4, (void*) 0x04));
  assert(upd_map_set(&map, (uint8_t*) "hoge", 4, (void*) 0x05));
  assert(upd_map_set(&map, (uint8_t*) "puyo", 4, (void*) 0x06));

  assert(upd_map_get(&map, (uint8_t*) "hoge", 4) == (void*) 0x05);
  assert(upd_map_get(&map, (uint8_t*) "fuga", 4) == (void*) 0x02);
  assert(upd_map_get(&map, (uint8_t*) "piyo", 4) == (void*) 0x03);
  assert(upd_map_get(&map, (uint8_t*) "poyo", 4) == (void*) 0x04);
  assert(upd_map_get(&map, (uint8_t*) "puyo", 4) == (void*) 0x06);

  assert(upd_map_get(&map, (uint8_t*) "hello", 5) == NULL);

  upd_map_clear(&map);
  assert(map == NULL);
}
#endif
