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
  upd_file_t* f   = req->file;
  dir_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  switch (req->type) {
  case UPD_REQ_DIR_LIST:
    req->dir.entries = (upd_req_dir_entries_t) {
      .n = ctx->children.n,
      .p = (upd_req_dir_entry_t**) ctx->children.p,
    };
    req->result = UPD_REQ_OK;
    req->cb(req);
    return true;

  case UPD_REQ_DIR_FIND: {
    size_t i;
    if (HEDLEY_LIKELY(entry_find_(ctx, &i, &req->dir.entry))) {
      req->dir.entry = *(upd_req_dir_entry_t*) ctx->children.p[i];
    } else {
      req->dir.entry = (upd_req_dir_entry_t) {0};
    }
    req->result = UPD_REQ_OK;
    req->cb(req);
  } return true;

  case UPD_REQ_DIR_ADD: {
    upd_req_dir_entry_t* re = &req->dir.entry;

    const bool valid =
      re->file != NULL && re->len && upd_path_validate_name(re->name, re->len);
    if (HEDLEY_UNLIKELY(!valid)) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }

    size_t i;
    if (HEDLEY_UNLIKELY(entry_find_by_name_(ctx, &i, re->name, re->len))) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }

    upd_req_dir_entry_t* e = entry_dup_(&req->dir.entry);

    req->dir.entry = (upd_req_dir_entry_t) {0};
    if (HEDLEY_UNLIKELY(e == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->children, e, SIZE_MAX))) {
      entry_delete_(e);
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    req->dir.entry = *e;
    req->result    = UPD_REQ_OK;
    req->cb(req);
  } return true;

  case UPD_REQ_DIR_NEWDIR: {
    upd_req_dir_entry_t re = req->dir.entry;
    req->dir.entry = (upd_req_dir_entry_t) {0};

    const bool valid = re.len && upd_path_validate_name(re.name, re.len);
    if (HEDLEY_UNLIKELY(!valid)) {
      req->result = UPD_REQ_INVALID;
      return false;
    }

    size_t i;
    if (HEDLEY_UNLIKELY(entry_find_by_name_(ctx, &i, re.name, re.len))) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }

    re.file = upd_file_new(&(upd_file_t) {
        .iso = iso,
        .driver = &upd_driver_dir,
      });
    if (HEDLEY_UNLIKELY(re.file == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    upd_req_dir_entry_t* e = entry_dup_(&re);
    upd_file_unref(re.file);
    if (HEDLEY_UNLIKELY(e == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }

    if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->children, e, SIZE_MAX))) {
      entry_delete_(e);
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    req->dir.entry = *e;
    req->result    = UPD_REQ_OK;
    req->cb(req);
  } return true;

  case UPD_REQ_DIR_RM: {
    size_t i;
    if (HEDLEY_UNLIKELY(!entry_find_(ctx, &i, &req->dir.entry))) {
      req->result = UPD_REQ_ABORTED;
      req->dir.entry = (upd_req_dir_entry_t) {0};
      return false;
    }
    upd_req_dir_entry_t* e = upd_array_remove(&ctx->children, i);
    req->dir.entry = *e;
    req->result    = UPD_REQ_OK;
    req->cb(req);
    entry_delete_(e);
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}


static upd_req_dir_entry_t* entry_dup_(const upd_req_dir_entry_t* src) {
  upd_req_dir_entry_t* e = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&e, sizeof(*e)+src->len+1))) {
    return NULL;
  }
  *e = *src;
  e->name = (uint8_t*) (e+1);

  utf8ncpy(e->name, src->name, src->len);
  e->name[src->len] = 0;

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
    if (HEDLEY_UNLIKELY(upd_streq(e->name, e->len, name, len))) {
      return true;
    }
  }
  return false;
}
