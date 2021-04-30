#pragma once

#include "common.h"


typedef struct upd_req_pathfind_t upd_req_pathfind_t;

struct upd_req_pathfind_t {
  /* Either one of the followings must be filled. */
  upd_iso_t*  iso;
  upd_file_t* base;

  const uint8_t* path;
  size_t         len;
  size_t         term;

  upd_req_t       req;
  upd_file_lock_t lock;

  void* udata;
  void
  (*cb)(
    upd_req_pathfind_t* pf);

  /* I cannot wait to win this match, friend! :) */
};


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


HEDLEY_NON_NULL(1)
void
upd_req_pathfind(
  upd_req_pathfind_t* pf);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline upd_req_pathfind_t* upd_req_pathfind_with_dup(
    const upd_req_pathfind_t* src) {
  upd_iso_t* iso = src->iso? src->iso: src->base->iso;
  assert(iso);

  upd_req_pathfind_t* pf = upd_iso_stack(iso, sizeof(*pf)+src->len);
  if (HEDLEY_UNLIKELY(pf == NULL)) {
    return NULL;
  }
  *pf = *src;

  pf->path = (uint8_t*) (pf+1);
  utf8ncpy((uint8_t*) pf->path, src->path, pf->len);

  upd_req_pathfind(pf);
  return pf;
}


#if defined(UPD_TEST)
static void upd_test_req_pathfind_cb_(upd_req_pathfind_t* pf) {
  assert(pf->len  == 0);
  assert(pf->base == pf->udata);
  upd_iso_unstack(pf->iso, pf);
}
static void upd_test_req(void) {
  const uint8_t* path_root = (uint8_t*) "/";
  assert(upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso   = upd_test.iso,
      .path  = path_root,
      .len   = utf8size_lazy(path_root),
      .udata = upd_file_get(upd_test.iso, UPD_FILE_ID_ROOT),
      .cb    = upd_test_req_pathfind_cb_,
    }));
}
#endif
