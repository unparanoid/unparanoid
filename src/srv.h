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


#if defined(UPD_TEST)
static void upd_test_srv(void) {
  upd_file_t* parallelism = upd_file_new(upd_test.iso, &upd_driver_program_parallelism);
  assert(parallelism);
  upd_file_t* http = upd_file_new(upd_test.iso, &upd_driver_program_http);
  assert(http);

  assert(upd_srv_new_tcp(
    upd_test.iso, parallelism, (uint8_t*) "0.0.0.0", 9999));
  assert(upd_srv_new_tcp(
    upd_test.iso, http, (uint8_t*) "0.0.0.0", 8080));

  upd_file_unref(parallelism);
  upd_file_unref(http);
}
#endif
