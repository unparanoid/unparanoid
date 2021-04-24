#pragma once

#include "common.h"


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_req(upd_req_t* req) {
  return req->file->driver->handle(req);
}
