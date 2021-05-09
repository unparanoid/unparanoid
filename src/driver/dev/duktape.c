#include "common.h"


static
bool
duktape_init_(
  upd_file_t* f);

static
void
duktape_deinit_(
  upd_file_t* f);

static
bool
duktape_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_dev_duktape = {
  .name = (uint8_t*) "upd.dev.duktape",
  .cats = (upd_req_cat_t[]) {
    0,
  },
  .init   = duktape_init_,
  .deinit = duktape_deinit_,
  .handle = duktape_handle_,
};


static bool duktape_init_(upd_file_t* f) {
  f->ctx = duk_create_heap(NULL, NULL, NULL, f, NULL);
  if (HEDLEY_UNLIKELY(f->ctx == NULL)) {
    return false;
  }
  return true;
}

static void duktape_deinit_(upd_file_t* f) {
  duk_destroy_heap(f->ctx);
}

static bool duktape_handle_(upd_req_t* req) {
  (void) req;
  return false;
}
