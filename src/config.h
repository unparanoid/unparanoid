#pragma once

#include "common.h"


typedef struct upd_config_apply_t upd_config_apply_t;

struct upd_config_apply_t {
  uv_fs_t fs;
  uv_file fd;

  upd_iso_t* iso;
  uint8_t    path[UPD_PATH_MAX];

  uint8_t* buf;

  yaml_document_t doc;
  size_t          refcnt;
  bool            ok;

  void* udata;
  void
  (*cb)(
    upd_config_apply_t* ap);
};


HEDLEY_NON_NULL(1)
void
upd_config_apply(
  upd_config_apply_t* ap);
