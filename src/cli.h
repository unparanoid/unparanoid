#pragma once

#include "common.h"


struct upd_cli_t {
  union uv_any_handle uv;

  upd_iso_t* iso;

  upd_file_t*      dir;
  upd_file_t*      prog;
  upd_file_t*      io;
  upd_file_watch_t watch;

  bool   deleted;
  size_t refcnt;

  struct {
    size_t   size;
    size_t   parsing;
    uint8_t* ptr;
  } buf;
};


HEDLEY_NON_NULL(1)
upd_cli_t*
upd_cli_new_tcp(
  upd_srv_t* srv);

HEDLEY_NON_NULL(1)
void
upd_cli_delete(
  upd_cli_t* cli);
