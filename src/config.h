#pragma once

#include "common.h"


HEDLEY_NON_NULL(1)
bool
upd_config_load_from_path(
  upd_iso_t*     iso,
  const uint8_t* path);


HEDLEY_NON_NULL(1)
static inline bool upd_config_load(upd_iso_t* iso) {
  return upd_config_load_from_path(iso, iso->path.working);
}


#if defined(UPD_TEST)
static void upd_test_config(void) {
  upd_config_load(upd_test.iso);
}
#endif
