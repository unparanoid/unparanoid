#pragma once

#include "common.h"


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline bool upd_req(upd_req_t* req) {
  return req->file->driver->handle(req);
}


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline upd_req_t* upd_req_with_dup(const upd_req_t* src) {
  upd_req_t* dst = upd_iso_stack(src->file->iso, sizeof(*dst));
  if (HEDLEY_UNLIKELY(dst == NULL)) {
    return NULL;
  }
  *dst = *src;

  if (HEDLEY_UNLIKELY(!upd_req(dst))) {
    upd_iso_unstack(src->file->iso, dst);
    return NULL;
  }
  return dst;
}
