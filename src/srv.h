#pragma once

#include "common.h"


typedef struct upd_srv_build_t upd_srv_build_t;


struct upd_srv_t {
  union uv_any_handle uv;

  upd_iso_t*  iso;
  upd_file_t* dir;
  upd_file_t* prog;

  uint8_t name[32];
};


struct upd_srv_build_t {
  upd_iso_t* iso;

  const uint8_t* path;
  size_t pathlen;

  const uint8_t* host;
  size_t hostlen;

  const uint8_t* addr;
  uint16_t       port;

  upd_file_t* prog;
  upd_file_t* dir;
  upd_srv_t*  srv;

  void* udata;

  void
  (*cb)(
    upd_srv_build_t* b);
};


HEDLEY_NON_NULL(1)
void
upd_srv_delete(
  upd_srv_t* srv);


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
bool
upd_srv_build(
  upd_srv_build_t* b);
