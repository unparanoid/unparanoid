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
};

struct task_t_ {
  ctx_t_*    ctx;
  upd_req_t* req;
  task_t_*   next;

  size_t refcnt;

  void
  (*cb)(
    task_t_* task);
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
void
syncdir_watch_cb_(
  upd_file_watch_t* w);


static
bool
task_queue_with_dup_(
  const task_t_* src);

static
bool
task_unref_(
  task_t_* task);


static
void
task_sync_n2u_cb_(
  task_t_* task);

static
void
task_sync_n2u_scandir_cb_(
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
  ctx_t_* ctx = req->file->ctx;

  switch (req->type) {
  case UPD_REQ_DIR_ACCESS:
    req->dir.access = (upd_req_dir_access_t) {
      .list = true,
      .find = true,
    };
    break;

  case UPD_REQ_DIR_LIST:
    req->dir.entries = (upd_req_dir_entries_t) {
      .p = (upd_req_dir_entry_t**) ctx->children.p,
      .n = ctx->children.n,
    };
    break;

  case UPD_REQ_DIR_FIND: {
    const upd_req_dir_entry_t needle = req->dir.entry;

    req->dir.entry = (upd_req_dir_entry_t) {0};
    for (size_t i = 0; i < ctx->children.n; ++i) {
      const upd_req_dir_entry_t* e = ctx->children.p[i];
      if (HEDLEY_UNLIKELY(utf8ncmp(e->name, needle.name, needle.len) == 0)) {
        req->dir.entry = *e;
        break;
      }
    }
  } break;

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
  task_queue_with_dup_(&(task_t_) {
      .ctx = ctx,
      .cb  = task_sync_n2u_cb_,
    });
}

static void syncdir_inherit_drvmap_(ctx_t_* ctx, drvmap_t_* drvmap) {
  if (HEDLEY_UNLIKELY(drvmap == NULL)) {
    return;
  }

  ++drvmap->refcnt;
  ctx->drvmap = drvmap;

  const bool sync = task_queue_with_dup_(&(task_t_) {
      .ctx = ctx,
      .cb  = task_sync_n2u_cb_,
    });
  if (HEDLEY_UNLIKELY(!sync)) {
    --drvmap->refcnt;
    return;
  }
}


static void syncdir_watch_cb_(upd_file_watch_t* w) {
  ctx_t_* ctx = w->udata;

  if (HEDLEY_UNLIKELY(w->event == UPD_FILE_UPDATE_N)) {
    const bool q = task_queue_with_dup_(&(task_t_) {
        .ctx = ctx,
        .cb  = task_sync_n2u_cb_,
      });
    if (HEDLEY_UNLIKELY(!q)) {
      upd_iso_msgf(ctx->file->iso,
        "failed to queue task for synchronizing native to upd\n");
      return;
    }
  }
}


static bool task_queue_with_dup_(const task_t_* src) {
  ctx_t_* ctx = src->ctx;

  task_t_* task = upd_iso_stack(ctx->file->iso, sizeof(*task));
  if (HEDLEY_UNLIKELY(task == NULL)) {
    return false;
  }
  *task = *src;
  task->refcnt = 1;

  task_t_* prev = ctx->last_task;

  ctx->last_task = task;
  if (HEDLEY_UNLIKELY(prev)) {
    prev->next = task;
  } else {
    task->cb(task);
  }
  upd_file_ref(ctx->file);
  return true;
}

static bool task_unref_(task_t_* task) {
  ctx_t_* ctx = task->ctx;

  if (HEDLEY_UNLIKELY(--task->refcnt == 0)) {
    if (HEDLEY_UNLIKELY(ctx->last_task == task)) {
      ctx->last_task = NULL;
    }
    if (HEDLEY_UNLIKELY(task->next)) {
      task->next->cb(task->next);
    }
    upd_iso_unstack(ctx->file->iso, task);
    upd_file_unref(ctx->file);
    return true;
  }
  return false;
}

static void task_sync_n2u_cb_(task_t_* task) {
  ctx_t_* ctx = task->ctx;

  if (HEDLEY_UNLIKELY(ctx->drvmap == NULL)) {
    goto ABORT;
  }

  uv_fs_t* fsreq = upd_iso_stack(ctx->file->iso, sizeof(*fsreq));
  if (HEDLEY_UNLIKELY(fsreq == NULL)) {
    goto ABORT;
  }
  *fsreq = (uv_fs_t) { .data = task, };

  uv_loop_t* loop = &ctx->file->iso->loop;
  const bool scandir = 0 <= uv_fs_scandir(
    loop, fsreq, (char*) ctx->file->npath, 0, task_sync_n2u_scandir_cb_);
  if (HEDLEY_UNLIKELY(!scandir)) {
    upd_iso_unstack(ctx->file->iso, fsreq);
    goto ABORT;
  }
  return;

ABORT:
  task_unref_(task);
}

static void task_sync_n2u_scandir_cb_(uv_fs_t* fsreq) {
  task_t_* task = fsreq->data;
  ctx_t_*  ctx  = task->ctx;

  bool* rm       = NULL;
  bool  modified = false;

  if (HEDLEY_UNLIKELY(fsreq->result < 0)) {
    goto EXIT;
  }

  const size_t prev_un = ctx->children.n;

  rm = upd_iso_stack(ctx->file->iso, sizeof(*rm)*prev_un);
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
      const size_t pathlen = cwk_path_join(
        (char*) ctx->file->npath, ne.name, NULL, 0);

      uint8_t* path = upd_iso_stack(ctx->file->iso, pathlen+1);
      if (HEDLEY_UNLIKELY(path == NULL)) {
        continue;
      }
      cwk_path_join((char*) ctx->file->npath, ne.name, (char*) path, pathlen+1);

      const upd_driver_t* d =
        dir? &upd_driver_syncdir:
        upd_driver_select(&ctx->drvmap->rules, (uint8_t*) path);
      if (HEDLEY_UNLIKELY(d == NULL)) {
        d = &upd_driver_bin_r;
      }

      upd_file_t* f =
        upd_file_new_from_normalized_npath(ctx->file->iso, d, path, pathlen);
      upd_iso_unstack(ctx->file->iso, path);
      if (HEDLEY_UNLIKELY(f == NULL)) {
        continue;
      }
      if (dir) {
        syncdir_inherit_drvmap_(f->ctx, ctx->drvmap);
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
        .file = f,
      };
      utf8ncpy(e->name, ne.name, len);
      e->name[len] = 0;

      if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->children, e, SIZE_MAX))) {
        upd_iso_unstack(ctx->file->iso, path);
        upd_file_unref(f);
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

  task_unref_(task);
}
