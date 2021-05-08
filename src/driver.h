#pragma once

#include "common.h"


extern const upd_driver_t upd_driver_dir;
extern const upd_driver_t upd_driver_syncdir;

extern const upd_driver_t upd_driver_program_http;
extern const upd_driver_t upd_driver_program_parallelism;


HEDLEY_NON_NULL(1, 2)
static inline bool upd_driver_register(
    upd_iso_t* iso, const upd_driver_t* driver) {
  const size_t len = utf8size_lazy(driver->name);
  if (HEDLEY_UNLIKELY(upd_driver_lookup(iso, driver->name, len))) {
    upd_iso_msgf(iso, "driver '%s' is already registered\n", driver->name);
    return false;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->drivers, (void*) driver, SIZE_MAX))) {
    upd_iso_msgf(iso, "driver registration failure because of memory error\n");
    return false;
  }
  return true;
}

HEDLEY_NON_NULL(1)
static inline const upd_driver_t* upd_driver_lookup(
    upd_iso_t* iso, const uint8_t* name, size_t len) {
  for (size_t i = 0; i < iso->drivers.n; ++i) {
    const upd_driver_t* d = iso->drivers.p[i];
    if (HEDLEY_UNLIKELY(utf8ncmp(d->name, name, len) == 0 && d->name[len] == 0)) {
      return d;
    }
  }
  return NULL;
}

HEDLEY_NON_NULL(1)
static inline const upd_driver_t* upd_driver_select(
    upd_iso_t*                      iso,
    const uint8_t*                  path,
    upd_map_of(const upd_driver_t*) extmap) {
  size_t      len;
  const char* ext;

  const bool has_ext = cwk_path_get_extension((char*) path, &ext, &len);
  if (HEDLEY_UNLIKELY(!has_ext || !len)) {
    return NULL;
  }
  --len;
  ++ext;

  const upd_driver_t* d =
    extmap? upd_map_get(&extmap, (uint8_t*) ext, len): NULL;
  return d? d: upd_map_get(&iso->extmap, (uint8_t*) ext, len);
}


#if defined(UPD_TEST)
static bool upd_test_driver_null_init_(upd_file_t* f) {
  (void) f;
  return true;
}
static void upd_test_driver_null_deinit_(upd_file_t* f) {
  (void) f;
}
static bool upd_test_driver_null_handle_(upd_req_t* req) {
  (void) req;
  return false;
}
static const upd_driver_t upd_test_driver_null_ = {
  .name   = (uint8_t*) "upd.test.driver.null",
  .cats   = (upd_req_cat_t[]) {0},
  .init   = upd_test_driver_null_init_,
  .deinit = upd_test_driver_null_deinit_,
  .handle = upd_test_driver_null_handle_,
};
static void upd_test_driver(void) {
  assert(upd_driver_register(upd_test.iso, &upd_test_driver_null_));
  assert(!upd_driver_register(upd_test.iso, &upd_test_driver_null_));

  const uint8_t* dname = upd_test_driver_null_.name;
  const upd_driver_t* d =
    upd_driver_lookup(upd_test.iso, dname, utf8size_lazy(dname));
  assert(d == &upd_test_driver_null_);

  assert(upd_map_set(&upd_test.iso->extmap, (uint8_t*) "testest", 7, (void*) d));
  assert(upd_driver_select(upd_test.iso, (uint8_t*) "hello.testest", NULL) == d);
}
#endif
