#include "common.h"


static const char* root_dirs_[] = {
  "prog",  /* for program   */
  "io",    /* for io stream */
};


static
void
iso_init_lock_cb_(
  upd_file_lock_t* l);

static
void
iso_init_add_dir_cb_(
  upd_req_t* req);


upd_iso_t* upd_iso_new(size_t stacksz) {
  /*  It's unnecessary to clean up resources in aborted,
   * because app exits immediately. */

  upd_iso_t* iso = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&iso, sizeof(*iso)+stacksz))) {
    return NULL;
  }
  *iso = (upd_iso_t) {
    .stack = {
      .size = stacksz,
      .ptr  = (uint8_t*) (iso+1),
    },
  };

  if (HEDLEY_UNLIKELY(0 > uv_loop_init(&iso->loop))) {
    return NULL;
  }

  if (HEDLEY_UNLIKELY(0 > uv_tty_init(&iso->loop, &iso->out, 1, 0))) {
    return NULL;
  }

  upd_file_t* root = upd_file_new(iso, &upd_driver_dir);
  if (HEDLEY_UNLIKELY(root == NULL)) {
    return NULL;
  }
  assert(root->id == UPD_FILE_ROOT);

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file = root,
      .cb   = iso_init_lock_cb_,
      .ex   = true,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    return NULL;
  }
  return iso;
}

static void iso_run_walk_cb_(uv_handle_t* handle, void* udata) {
  (void) udata;
  if (HEDLEY_LIKELY(!uv_is_closing(handle))) {
    uv_close(handle, NULL);
  }
}
upd_iso_status_t upd_iso_run(upd_iso_t* iso) {
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  uv_walk(&iso->loop, iso_run_walk_cb_, NULL);
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  if (HEDLEY_UNLIKELY(0 > uv_loop_close(&iso->loop))) {
    return UPD_ISO_PANIC;
  }
  upd_array_clear(&iso->drivers);

  assert(iso->files.n);
  upd_file_unref(iso->files.p[0]);

  assert(iso->stack.refcnt == 0);
  assert(iso->files.n == 0);
  assert(iso->srv.n   == 0);
  assert(iso->cli.n   == 0);

  const upd_iso_status_t ret = iso->status;
  upd_free(&iso);
  return ret;
}

void upd_iso_close_all_conn(upd_iso_t* iso) {
  while (iso->srv.n) {
    upd_srv_delete(iso->srv.p[0]);
  }
  while (iso->cli.n) {
    upd_cli_delete(iso->cli.p[0]);
  }
}


static void iso_init_lock_cb_(upd_file_lock_t* l) {
  upd_iso_t* iso = l->file->iso;

  const bool ok = l->ok;
  upd_iso_unstack(iso, l);

  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, "cannot setup iso since failure of acquiring lock\n");
    return;
  }

  assert(iso->files.n);
  upd_file_t* root = iso->files.p[0];

  for (size_t i = 0; i < sizeof(root_dirs_)/sizeof(root_dirs_[0]); ++i) {
    upd_file_t* dir = upd_file_new(iso, &upd_driver_dir);
    if (HEDLEY_UNLIKELY(dir == NULL)) {
      continue;
    }
    const bool add = upd_req_with_dup(&(upd_req_t) {
        .file = root,
        .type = UPD_REQ_DIR_ADD,
        .dir  = { .entry = {
          .file = dir,
          .name = (uint8_t*) root_dirs_[i],
          .len  = utf8size_lazy(root_dirs_[i]),
        }, },
        .cb = iso_init_add_dir_cb_,
      });
    if (HEDLEY_UNLIKELY(!add)) {
      continue;
    }
  }
}

static void iso_init_add_dir_cb_(upd_req_t* req) {
  upd_file_unref(req->dir.entry.file);
  upd_iso_unstack(req->file->iso, req);
}
