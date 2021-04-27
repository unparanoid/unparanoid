#include "common.h"


typedef struct dir_t_ {
  upd_array_of(upd_req_dir_entry_t*) children;
} dir_t_;


static
bool
dir_init_(
  upd_file_t* f);

static
void
dir_deinit_(
  upd_file_t* f);

static
bool
dir_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_dir = {
  .name = (uint8_t*) "upd.dir",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_DIR,
    0,
  },
  .init   = dir_init_,
  .deinit = dir_deinit_,
  .handle = dir_handle_,
};


static
upd_req_dir_entry_t*
entry_dup_(
  const upd_req_dir_entry_t* src);

static
void
entry_delete_(
  upd_req_dir_entry_t* e);

static
bool
entry_find_(
  dir_t_*                    ctx,
  size_t*                    i,
  const upd_req_dir_entry_t* e);

static
bool
entry_find_by_file_(
  dir_t_*     ctx,
  size_t*     i,
  upd_file_t* f);

static
bool
entry_find_by_name_(
  dir_t_*        ctx,
  size_t*        i,
  const uint8_t* name,
  size_t         len);


static bool dir_init_(upd_file_t* f) {
  dir_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (dir_t_) {0};
  f->ctx = ctx;
  return true;
}

static void dir_deinit_(upd_file_t* f) {
  dir_t_* ctx = f->ctx;
  for (size_t i = 0; i < ctx->children.n; ++i) {
    entry_delete_(ctx->children.p[i]);
  }
  upd_array_clear(&ctx->children);
  upd_free(&ctx);
}

static bool dir_handle_(upd_req_t* req) {
  dir_t_* ctx = req->file->ctx;

  switch (req->type) {
  case UPD_REQ_DIR_LIST:
    req->dir.entries = (upd_req_dir_entry_t**) ctx->children.p;
    break;

  case UPD_REQ_DIR_FIND: {
    size_t i;
    if (HEDLEY_UNLIKELY(!entry_find_(ctx, &i, &req->dir.entry))) {
      req->dir.entry = (upd_req_dir_entry_t) {0};
      return false;
    }
    req->dir.entry = *(upd_req_dir_entry_t*) ctx->children.p[i];
  } break;

  case UPD_REQ_DIR_ADD: {
    upd_req_dir_entry_t* e = entry_dup_(&req->dir.entry);

    req->dir.entry = (upd_req_dir_entry_t) {0};
    if (HEDLEY_UNLIKELY(e == NULL)) {
      return false;
    }
    if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->children, e, SIZE_MAX))) {
      upd_free(&e);
      return false;
    }
    req->dir.entry = *e;
  } break;

  case UPD_REQ_DIR_RM: {
    size_t i;
    if (HEDLEY_UNLIKELY(!entry_find_(ctx, &i, &req->dir.entry))) {
      req->dir.entry = (upd_req_dir_entry_t) {0};
      return false;
    }
    entry_delete_(upd_array_remove(&ctx->children, i));
  } break;

  default:
    return false;
  }

  req->cb(req);
  return true;
}


static upd_req_dir_entry_t* entry_dup_(const upd_req_dir_entry_t* src) {
  upd_req_dir_entry_t* e = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&e, sizeof(*e)+src->len+1))) {
    return NULL;
  }
  *e = (upd_req_dir_entry_t) {
    .name = (uint8_t*) (e+1),
    .len  = src->len,
    .file = src->file,
  };
  utf8ncpy(e->name, src->name, e->len);
  e->name[e->len] = 0;
  upd_file_ref(e->file);
  return e;
}

static void entry_delete_(upd_req_dir_entry_t* e) {
  upd_file_unref(e->file);
  upd_free(&e);
}

static bool entry_find_(dir_t_* ctx, size_t* i, const upd_req_dir_entry_t* e) {
  return e->file?
    entry_find_by_file_(ctx, i, e->file):
    entry_find_by_name_(ctx, i, e->name, e->len);
}

static bool entry_find_by_file_(dir_t_* ctx, size_t* i, upd_file_t* f) {
  for (*i = 0; *i < ctx->children.n; ++*i) {
    const upd_req_dir_entry_t* e = ctx->children.p[*i];
    if (HEDLEY_UNLIKELY(e->file == f)) {
      return true;
    }
  }
  return false;
}

static bool entry_find_by_name_(
    dir_t_* ctx, size_t* i, const uint8_t* name, size_t len) {
  for (*i = 0; *i < ctx->children.n; ++*i) {
    const upd_req_dir_entry_t* e = ctx->children.p[*i];
    if (HEDLEY_UNLIKELY(e->len == len && utf8ncmp(e->name, name, len) == 0)) {
      return true;
    }
  }
  return false;
}
