#include "common.h"


static
void
pathfind_(
  upd_req_pathfind_t* pf);

static
void
pathfind_lock_cb_(
  upd_file_lock_t* lock);

static
void
pathfind_find_cb_(
  upd_req_t* req);

static
void
pathfind_add_cb_(
  upd_req_t* req);


void upd_req_pathfind(upd_req_pathfind_t* pf) {
  if (pf->len && pf->path[0] == '/') {
    pf->base = NULL;
  }
  if (!pf->base) {
    pf->base = upd_file_get(pf->iso, UPD_FILE_ID_ROOT);
  }
  if (!pf->iso) {
    pf->iso = pf->base->iso;
  }
  pathfind_(pf);
}
static void pathfind_(upd_req_pathfind_t* pf) {
  while (pf->len && pf->path[0] == '/') {
    ++pf->path;
    --pf->len;
  }
  pf->term = 0;
  while (pf->term < pf->len && pf->path[pf->term] != '/') {
    ++pf->term;
  }
  if (!pf->base) {
    pf->base = upd_file_get(pf->iso, UPD_FILE_ID_ROOT);
  }
  if (!pf->len) {
    pf->cb(pf);
    return;
  }

  pf->lock = (upd_file_lock_t) {
    .file  = pf->base,
    .udata = pf,
    .cb    = pathfind_lock_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&pf->lock))) {
    pf->cb(pf);
    return;
  }
}
static void pathfind_lock_cb_(upd_file_lock_t* lock) {
  upd_req_pathfind_t* pf = lock->udata;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }
  pf->req = (upd_req_t) {
    .file = pf->base,
    .type = UPD_REQ_DIR_FIND,
    .dir  = { .entry = {
      .name = (uint8_t*) pf->path,
      .len  = pf->term,
    }, },
    .udata = pf,
    .cb    = pathfind_find_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&pf->req))) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(&pf->lock);
  pf->cb(pf);
}
static void pathfind_find_cb_(upd_req_t* req) {
  upd_req_pathfind_t* pf = req->udata;

  if (HEDLEY_UNLIKELY(req->dir.entry.file == NULL)) {
    if (pf->create) {
      pf->req = (upd_req_t) {
        .file = pf->base,
        .type = UPD_REQ_DIR_NEWDIR,
        .dir  = { .entry = {
          .name = pf->path,
          .len  = pf->term,
        }, },
        .udata = pf,
        .cb    = pathfind_add_cb_,
      };
      const bool newdir = upd_req(&pf->req);
      if (HEDLEY_LIKELY(newdir)) {
        return;
      }
    }
    upd_file_unlock(&pf->lock);
    pf->cb(pf);
    return;
  }

  upd_file_unlock(&pf->lock);
  pf->base  = req->dir.entry.file;
  pf->path += pf->term;
  pf->len  -= pf->term;
  pathfind_(pf);
}
static void pathfind_add_cb_(upd_req_t* req) {
  upd_req_pathfind_t* pf = req->udata;

  upd_file_unlock(&pf->lock);

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    pf->cb(pf);
    return;
  }

  pf->base  = req->dir.entry.file;
  pf->path += pf->term;
  pf->len  -= pf->term;
  pathfind_(pf);
}
