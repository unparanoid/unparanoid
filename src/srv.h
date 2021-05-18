#pragma once

#include "common.h"


struct upd_srv_t {
  union uv_any_handle uv;

  upd_iso_t*  iso;
  upd_file_t* dir;
  upd_file_t* prog;

  uint8_t* name;
};


HEDLEY_NON_NULL(1)
upd_srv_t*
upd_srv_new_tcp(
  upd_iso_t*     iso,
  upd_file_t*    prog,
  const uint8_t* addr,
  uint16_t       port);

HEDLEY_NON_NULL(1)
void
upd_srv_delete(
  upd_srv_t* srv);
