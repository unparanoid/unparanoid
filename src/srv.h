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
  const uint8_t* dname = (void*) "upd.test.driver.null";

  upd_file_t* f = upd_file_new_from_driver_name(
    upd_test.iso, dname, utf8size_lazy(dname));
  assert(f);

  upd_srv_t* srv = upd_srv_new_tcp(
    upd_test.iso, f, (uint8_t*) "0.0.0.0", 9999);
  assert(srv);

  upd_file_unref(f);
}
#endif
