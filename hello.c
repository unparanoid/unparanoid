/* 
 * External Driver Example
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPD_EXTERNAL_DRIVER
#include "libupd.h"


static
bool
test_init_(
  upd_file_t* f);

static
void
test_deinit_(
  upd_file_t* f);

static
bool
test_handle_(
  upd_req_t* req);


static const upd_driver_t test_drv_ = {
  .name = (uint8_t*) "falsycat.test",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_BIN,
    0,
  },
  .init   = test_init_,
  .deinit = test_deinit_,
  .handle = test_handle_,
};


upd_external_t upd = {
  .drivers = (const upd_driver_t*[]) {
    &test_drv_,
    NULL,
  },
};


static bool test_init_(upd_file_t* f) {
  upd_iso_msg(f->iso, (uint8_t*) "Hello World!\n", 13);
  return true;
}
static void test_deinit_(upd_file_t* f) {
  upd_iso_msg(f->iso, (uint8_t*) "bye!\n", 5);
  (void) f;
}
static bool test_handle_(upd_req_t* req) {
  switch (req->type) {
  case UPD_REQ_BIN_ACCESS:
    req->bin.access = (upd_req_bin_access_t) {
      .read = true,
    };
    break;
  case UPD_REQ_BIN_READ:
    if (req->bin.rw.offset) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    req->bin.rw = (upd_req_bin_rw_t) {
      .buf  = (uint8_t*) "Hello World!",
      .size = 12,
    };
    break;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}
