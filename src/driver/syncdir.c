#include "common.h"


typedef struct ctx_t_    ctx_t_;
typedef struct task_t_   task_t_;
typedef struct drvmap_t_ drvmap_t_;

struct ctx_t_ {
  upd_file_t*      file;
  upd_file_watch_t watch;

  uint64_t last_scandir;

  upd_array_of(upd_req_dir_entity_t*) children;

  task_t_*   last_task;
  drvmap_t_* drvmap;

  bool                     busy;
  upd_array_of(upd_req_t*) reqs;
};

struct drvmap_t_ {
  size_t refcnt;
  upd_array_of(upd_driver_rule_t*) rules;
};


static
bool
syncdir_init_(
  upd_file_t* file);

static
void
syncdir_deinit_(
  upd_file_t* file);

static
bool
syncdir_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_syncdir = {
  .name = (uint8_t*) "upd.syncdir",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_DIR,
    0,
  },
  .flags = {
    .npoll = true,
  },
  .init   = syncdir_init_,
  .deinit = syncdir_deinit_,
  .handle = syncdir_handle_,
};


static
void
syncdir_inherit_drvmap_(
  ctx_t_*    ctx,
  drvmap_t_* drvmap);

static
size_t
syncdir_create_child_path_(
  ctx_t_*        ctx,
  uint8_t**      dst,
  const uint8_t* name,
  size_t         len);

static
bool
syncdir_find_(
  ctx_t_*              ctx,
  upd_req_dir_entry_t* e);

static
bool
syncdir_sync_n2u_(
  ctx_t_*    ctx,
  upd_req_t* req);

static
void
syncdir_sync_finalize_(
  ctx_t_* ctx);


static
void
syncdir_watch_cb_(
  upd_file_watch_t* w);

static
void
syncdir_open_cb_(
  uv_fs_t* fsreq);

static
void
syncdir_close_cb_(
  uv_fs_t* fsreq);

static
void
syncdir_mkdir_cb_(
  uv_fs_t* fsreq);

static
void
syncdir_sync_n2u_lock_cb_(
  upd_file_lock_t* lock);

static
void
syncdir_sync_n2u_scandir_cb_(
  uv_fs_t* fsreq);


static bool syncdir_init_(upd_file_t* file) {
  if (HEDLEY_UNLIKELY(file->npath == NULL)) {
    return false;
  }
  ctx_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (ctx_t_) {
    .watch = {
      .file  = file,
      .udata = ctx,
      .cb    = syncdir_watch_cb_,
    },
    .file = file,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    return false;
  }
  file->ctx = ctx;
  return true;
}

static void syncdir_deinit_(upd_file_t* file) {
  ctx_t_* ctx = file->ctx;
  for (size_t i = 0; i < ctx->children.n; ++i) {
    upd_req_dir_entry_t* e = ctx->children.p[i];
    upd_file_unref(e->file);
    upd_free(&e);
  }
  upd_array_clear(&ctx->children);

  if (HEDLEY_LIKELY(ctx->drvmap)) {
    if (HEDLEY_UNLIKELY(--ctx->drvmap->refcnt == 0)) {
      for (size_t i = 0; i < ctx->drvmap->rules.n; ++i) {
        upd_free(&ctx->drvmap->rules.p[i]);
      }
      upd_array_clear(&ctx->drvmap->rules);
      upd_free(&ctx->drvmap);
    }
  }

  upd_free(&ctx);
}

static bool syncdir_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  ctx_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  switch (req->type) {
  case UPD_REQ_DIR_ACCESS:
    req->dir.access = (upd_req_dir_access_t) {
      .list = true,
      .find = true,
      .new  = true,
    };
    break;

  case UPD_REQ_DIR_LIST:
    req->dir.entries = (upd_req_dir_entries_t) {
      .p = (upd_req_dir_entry_t**) ctx->children.p,
      .n = ctx->children.n,
    };
    break;

  case UPD_REQ_DIR_FIND: {
    syncdir_find_(ctx, &req->dir.entry);
  } break;

  case UPD_REQ_DIR_NEW: {
    const upd_req_dir_new_t*   n = &req->dir.new;
    const upd_req_dir_entry_t* e = &n->entry;

    uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
    if (HEDLEY_UNLIKELY(fsreq == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    *fsreq = (uv_fs_t) { .data = req, };

    uint8_t* path;
    syncdir_create_child_path_(ctx, &path, e->name, e->len);

    upd_file_ref(f);

    bool open;
    if (n->dir) {
      open = 0 <= uv_fs_mkdir(
        &iso->loop, fsreq, (char*) path, S_IFDIR, syncdir_mkdir_cb_);
    } else {
      open = 0 <= uv_fs_open(
        &iso->loop, fsreq, (char*) path, 0, O_WRONLY, syncdir_open_cb_);
    }
    upd_iso_unstack(iso, path);
    if (HEDLEY_UNLIKELY(!open)) {
      upd_file_unref(f);
      req->result = UPD_REQ_ABORTED;
      return false;
    }
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}


void upd_driver_syncdir_set_rules(
    upd_file_t* file, const upd_array_of(upd_driver_rule_t*)* rules) {
  assert(file->driver == &upd_driver_syncdir);

  ctx_t_* ctx = file->ctx;
  assert(ctx->drvmap == NULL);

  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx->drvmap, sizeof(*ctx->drvmap)))) {
    return;
  }
  *ctx->drvmap = (drvmap_t_) {
    .refcnt = 1,
    .rules  = *rules,
  };
  syncdir_sync_n2u_(ctx, NULL);
}

static void syncdir_inherit_drvmap_(ctx_t_* ctx, drvmap_t_* drvmap) {
  if (HEDLEY_UNLIKELY(drvmap == NULL)) {
    return;
  }
  ++drvmap->refcnt;
  ctx->drvmap = drvmap;
  if (HEDLEY_UNLIKELY(!syncdir_sync_n2u_(ctx, NULL))) {
    --drvmap->refcnt;
    ctx->drvmap = NULL;
    return;
  }
}

static size_t syncdir_create_child_path_(
    ctx_t_* ctx, uint8_t** dst, const uint8_t* name, size_t len) {
  upd_file_t* f   = ctx->file;
  upd_iso_t*  iso = f->iso;

  uint8_t* term = upd_iso_stack(iso, len+1);
  if (HEDLEY_UNLIKELY(term == NULL)) {
    return 0;
  }
  utf8ncpy(term, name, len);
  term[len] = 0;

  const size_t dstlen = cwk_path_join((char*) f->npath, (char*) term, NULL, 0);
  *dst = upd_iso_stack(iso, dstlen+1);
  if (HEDLEY_UNLIKELY(*dst == NULL)) {
    upd_iso_unstack(iso, term);
    return 0;
  }
  cwk_path_join((char*) f->npath, (char*) term, (char*) *dst, dstlen+1);
  upd_iso_unstack(iso, term);

  return dstlen;
}

static bool syncdir_find_(ctx_t_* ctx, upd_req_dir_entry_t* e) {
  for (size_t i = 0; i < ctx->children.n; ++i) {
    const upd_req_dir_entry_t* g = ctx->children.p[i];
    const bool match =
      g->len == e->len && utf8ncmp(e->name, g->name, g->len) == 0;
    if (HEDLEY_UNLIKELY(match)) {
      *e = *g;
      return true;
    }
  }
  *e = (upd_req_dir_entry_t) {0};
  return false;
}

static bool syncdir_sync_n2u_(ctx_t_* ctx, upd_req_t* req) {
  upd_file_t* f = ctx->file;

  if (HEDLEY_UNLIKELY(req)) {
    if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->reqs, req, SIZE_MAX))) {
      return false;
    }
  }
  if (HEDLEY_UNLIKELY(ctx->busy)) {
    return true;
  }

  ctx->busy = true;

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .udata = ctx,
      .cb    = syncdir_sync_n2u_lock_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    syncdir_sync_finalize_(ctx);
  }
  return true;
}

static void syncdir_sync_finalize_(ctx_t_* ctx) {
  assert(ctx->busy);

  for (size_t i = 0; i < ctx->reqs.n; ++i) {
    upd_req_t* req = ctx->reqs.p[i];
    assert(req->type == UPD_REQ_DIR_NEW);

    req->result =
      syncdir_find_(ctx, &req->dir.new.entry)? UPD_REQ_OK: UPD_REQ_ABORTED;
    req->cb(req);
  }
  ctx->busy = false;
}


static void syncdir_watch_cb_(upd_file_watch_t* w) {
  ctx_t_* ctx = w->udata;

  if (HEDLEY_UNLIKELY(w->event == UPD_FILE_UPDATE_N)) {
    if (HEDLEY_UNLIKELY(!syncdir_sync_n2u_(ctx, NULL))) {
      upd_iso_msgf(ctx->file->iso,
        "failed to queue task for synchronizing native to upd\n");
      return;
    }
  }
}

static void syncdir_open_cb_(uv_fs_t* fsreq) {
  upd_req_t*  req = fsreq->data;
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    req->result = UPD_REQ_ABORTED;
    req->cb(req);
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!syncdir_sync_n2u_(ctx, req))) {
    req->result = UPD_REQ_ABORTED;
    req->cb(req);
  }

  fsreq->data = ctx;
  const bool close = uv_fs_close(&iso->loop, fsreq, result, syncdir_close_cb_);
  if (HEDLEY_UNLIKELY(!close)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_iso_unstack(iso, fsreq);
  upd_file_unref(f);
}

static void syncdir_close_cb_(uv_fs_t* fsreq) {
  ctx_t_*     ctx = fsreq->data;
  upd_file_t* f   = ctx->file;

  uv_fs_req_cleanup(fsreq);
  upd_file_unref(f);
}

static void syncdir_mkdir_cb_(uv_fs_t* fsreq) {
  upd_req_t*  req = fsreq->data;
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!syncdir_sync_n2u_(ctx, req))) {
    goto ABORT;
  }
  return;

ABORT:
  req->result = UPD_REQ_ABORTED;
  req->cb(req);

  upd_iso_unstack(iso, fsreq);
  upd_file_unref(f);
}

static void syncdir_sync_n2u_lock_cb_(upd_file_lock_t* lock) {
  ctx_t_*     ctx  = lock->udata;
  upd_file_t* f    = ctx->file;
  upd_iso_t*  iso  = f->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(ctx->drvmap == NULL)) {
    goto ABORT;
  }

  uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
  if (HEDLEY_UNLIKELY(fsreq == NULL)) {
    goto ABORT;
  }
  *fsreq = (uv_fs_t) { .data = lock, };

  const bool scandir = 0 <= uv_fs_scandir(
    &iso->loop, fsreq, (char*) f->npath, 0, syncdir_sync_n2u_scandir_cb_);
  if (HEDLEY_UNLIKELY(!scandir)) {
    upd_iso_unstack(iso, fsreq);
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  syncdir_sync_finalize_(ctx);
}

static void syncdir_sync_n2u_scandir_cb_(uv_fs_t* fsreq) {
  upd_file_lock_t* lock = fsreq->data;
  ctx_t_*          ctx  = lock->udata;
  upd_file_t*      f    = ctx->file;
  upd_iso_t*       iso  = f->iso;

  bool* rm       = NULL;
  bool  modified = false;

  if (HEDLEY_UNLIKELY(fsreq->result < 0)) {
    goto EXIT;
  }

  const size_t prev_un = ctx->children.n;

  rm = upd_iso_stack(iso, sizeof(*rm)*prev_un);
  if (HEDLEY_UNLIKELY(rm == NULL)) {
    goto EXIT;
  }
  for (size_t i = 0; i < prev_un; ++i) {
    rm[i] = true;
  }

  for (size_t n = 0; n < (size_t) fsreq->result; ++n) {
    uv_dirent_t ne;
    if (HEDLEY_UNLIKELY(0 > uv_fs_scandir_next(fsreq, &ne))) {
      break;
    }

    const bool dir = ne.type == UV_DIRENT_DIR;
    if (HEDLEY_UNLIKELY(!dir && ne.type != UV_DIRENT_FILE)) {
      continue;  /* We can't handle others because they depends on OS. */
    }

    bool found = false;
    for (size_t u = 0; u < ctx->children.n; ++u) {
      upd_req_dir_entry_t* ue = ctx->children.p[u];
      if (HEDLEY_UNLIKELY(utf8cmp(ne.name, ue->name) == 0)) {
        found = true;
        if (HEDLEY_LIKELY(u < prev_un)) {
          rm[u] = false;
        }
        break;
      }
    }

    if (HEDLEY_UNLIKELY(!found)) {
      uint8_t* path;
      const size_t pathlen = syncdir_create_child_path_(
        ctx, &path, (uint8_t*) ne.name, utf8size_lazy(ne.name));

      const upd_driver_t* d;
      if (dir) {
        d = &upd_driver_syncdir;
      } else {
        d = upd_driver_select(&ctx->drvmap->rules, (uint8_t*) path);
        if (HEDLEY_UNLIKELY(d == NULL)) {
          d = &upd_driver_bin_r;
        }
      }

      upd_file_t* fc =
        upd_file_new_from_normalized_npath(iso, d, path, pathlen);
      upd_iso_unstack(iso, path);
      if (HEDLEY_UNLIKELY(fc == NULL)) {
        continue;
      }
      if (dir) {
        syncdir_inherit_drvmap_(fc->ctx, ctx->drvmap);
      }

      const size_t         len = utf8size_lazy(ne.name);
      upd_req_dir_entry_t* e   = NULL;
      if (HEDLEY_UNLIKELY(!upd_malloc(&e, sizeof(*e)+len+1))) {
        upd_file_unref(f);
        continue;
      }
      *e = (upd_req_dir_entry_t) {
        .name = (uint8_t*) (e+1),
        .len  = len,
        .file = fc,
      };
      utf8ncpy(e->name, ne.name, len);
      e->name[len] = 0;

      if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->children, e, SIZE_MAX))) {
        upd_iso_unstack(iso, path);
        upd_file_unref(fc);
        upd_free(&e);
        continue;
      }
      modified = true;
    }
  }

  for (size_t i = prev_un; i > 0;) {
    --i;

    if (HEDLEY_UNLIKELY(rm[i])) {
      upd_req_dir_entry_t* e = upd_array_remove(&ctx->children, i);
      if (HEDLEY_UNLIKELY(e == NULL)) {
        continue;
      }
      upd_file_unref(e->file);
      upd_free(&e);
      modified = true;
    }
  }

  ctx->last_scandir = upd_iso_now(ctx->file->iso);

EXIT:
  if (HEDLEY_LIKELY(rm)) {
    upd_iso_unstack(ctx->file->iso, rm);
  }

  uv_fs_req_cleanup(fsreq);
  upd_iso_unstack(ctx->file->iso, fsreq);

  if (HEDLEY_UNLIKELY(modified)) {
    upd_file_trigger(ctx->file, UPD_FILE_UPDATE);
  }

  syncdir_sync_finalize_(ctx);

  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);
}
