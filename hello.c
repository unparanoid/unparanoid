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
    UPD_REQ_STREAM,
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
  case UPD_REQ_STREAM_ACCESS:
    req->stream.access = (upd_req_stream_access_t) {
      .read = true,
    };
    break;
  case UPD_REQ_STREAM_READ:
    if (req->stream.io.offset) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    req->stream.io = (upd_req_stream_io_t) {
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
