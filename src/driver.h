#pragma once

#include "common.h"


typedef struct upd_driver_rule_t          upd_driver_rule_t;
typedef struct upd_driver_load_external_t upd_driver_load_external_t;


struct upd_driver_load_external_t {
  upd_iso_t* iso;

  const uint8_t* npath;
  size_t         npathlen;

  const char* err;
  bool        ok;
  uv_lib_t*   lib;

  void* udata;

  void
  (*cb)(
    upd_driver_load_external_t* load);
};


extern const upd_driver_t upd_driver_bin;
extern const upd_driver_t upd_driver_dir;
extern const upd_driver_t upd_driver_factory;
extern const upd_driver_t upd_driver_syncdir;
extern const upd_driver_t upd_driver_srv;
extern const upd_driver_t upd_driver_srv_tcp;


HEDLEY_NON_NULL(1)
void
upd_driver_setup(
  upd_iso_t* iso);

HEDLEY_NON_NULL(1)
bool
upd_driver_load_external(
  upd_driver_load_external_t* load);


HEDLEY_NON_NULL(1)
upd_file_t*
upd_driver_srv_tcp_new(
  upd_file_t*    prog,
  const uint8_t* host,
  uint16_t       port);


HEDLEY_NON_NULL(1, 2)
HEDLEY_WARN_UNUSED_RESULT
static inline
bool
upd_driver_register(
  upd_iso_t*          iso,
  const upd_driver_t* driver);


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

static inline const upd_driver_t* upd_driver_lookup(
    upd_iso_t* iso, const uint8_t* name, size_t len) {
  for (size_t i = 0; i < iso->drivers.n; ++i) {
    const upd_driver_t* d = iso->drivers.p[i];
    if (HEDLEY_UNLIKELY(upd_streq_c(d->name, name, len))) {
      return d;
    }
  }
  return NULL;
}
