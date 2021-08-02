#pragma once

#include "common.h"


typedef enum upd_config_feature_t {
  UPD_CONFIG_REQUIRE = 1 << 0,
  UPD_CONFIG_IMPORT  = 1 << 1,
  UPD_CONFIG_DRIVER  = 1 << 2,
  UPD_CONFIG_FILE    = 1 << 3,

  UPD_CONFIG_FULL =
    UPD_CONFIG_REQUIRE |
    UPD_CONFIG_IMPORT  |
    UPD_CONFIG_DRIVER  |
    UPD_CONFIG_FILE,

  UPD_CONFIG_SECURE =
    UPD_CONFIG_REQUIRE |
    UPD_CONFIG_DRIVER  |
    UPD_CONFIG_FILE,
} upd_config_feature_t;

typedef uint8_t upd_config_features_t;

typedef struct upd_config_load_t upd_config_load_t;

struct upd_config_load_t {
  upd_iso_t* iso;
  uint8_t*   path;

  upd_config_features_t feats;

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
