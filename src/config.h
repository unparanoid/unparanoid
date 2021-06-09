#pragma once

#include "common.h"


typedef struct upd_config_load_t upd_config_load_t;

struct upd_config_load_t {
  upd_iso_t* iso;
  uint8_t*   path;

  unsigned ok : 1;

  void* udata;
  void
  (*cb)(
    upd_config_load_t* load);
};

HEDLEY_NON_NULL(1)
bool
upd_config_load(
  upd_config_load_t* load);

HEDLEY_NON_NULL(1)
static inline
bool
upd_config_load_with_dup(
  const upd_config_load_t* src);


static inline bool upd_config_load_with_dup(const upd_config_load_t* src) {
  upd_iso_t* iso = src->iso;

  upd_config_load_t* load = upd_iso_stack(iso, sizeof(*load));
  if (HEDLEY_UNLIKELY(load == NULL)) {
    return false;
  }
  *load = *src;

  if (HEDLEY_UNLIKELY(!upd_config_load(load))) {
    upd_iso_unstack(iso, load);
    return false;
  }
  return true;
}
