#include "common.h"


typedef struct setup_t_ {
  upd_file_lock_t lock;

  upd_iso_t* iso;
  size_t     refcnt;
} setup_t_;


static const struct {
  const char* name;
  uint64_t    id;
} root_dirs_[] = {
  { "dev",  UPD_FILE_ID_DEV,  },
  { "io",   UPD_FILE_ID_IO,   },
  { "prog", UPD_FILE_ID_PROG, },
};


static
bool
iso_get_paths_(
  upd_iso_t* iso);


static
void
iso_setup_unref_(
  setup_t_* setup);

static
upd_file_t*
iso_setup_install_(
  setup_t_*           setup,
  upd_file_t*         parent,
  const upd_driver_t* drv,
  const char*         name);


static
void
iso_setup_lock_cb_(
  upd_file_lock_t* lock);

static
void
iso_setup_install_cb_(
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

  if (HEDLEY_UNLIKELY(!iso_get_paths_(iso))) {
    return NULL;
  }

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
  assert(root->id == UPD_FILE_ID_ROOT);

  setup_t_* setup = upd_iso_stack(iso, sizeof(*setup));
  if (HEDLEY_UNLIKELY(setup == NULL)) {
    return NULL;
  }

  *setup = (setup_t_) {
    .lock = {
      .file = root,
      .ex   = true,
      .cb   = iso_setup_lock_cb_,
    },
    .iso = iso,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&setup->lock))) {
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

  upd_iso_close_all_conn(iso);
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


static bool iso_get_paths_(upd_iso_t* iso) {
  size_t len = UPD_PATH_MAX;
  const bool exepath = 0 <= uv_exepath((char*) iso->path.runtime, &len);
  if (HEDLEY_UNLIKELY(!exepath || len >= UPD_PATH_MAX)) {
    return false;
  }
  cwk_path_get_dirname((char*) iso->path.runtime, &len);
  iso->path.runtime[len] = 0;
  cwk_path_normalize(
    (char*) iso->path.runtime, (char*) iso->path.runtime, UPD_PATH_MAX);

  len = UPD_PATH_MAX;
  const bool cwd = 0 <= uv_cwd((char*) iso->path.working, &len);
  if (HEDLEY_UNLIKELY(!cwd || len >= UPD_PATH_MAX)) {
    return false;
  }
  cwk_path_normalize(
    (char*) iso->path.working, (char*) iso->path.working, UPD_PATH_MAX);
  return true;
}


static void iso_setup_unref_(setup_t_* setup) {
  if (HEDLEY_UNLIKELY(--setup->refcnt == 0)) {
    upd_file_unlock(&setup->lock);

    upd_driver_setup_iso(setup->iso);
    upd_iso_unstack(setup->iso, setup);
  }
}

static upd_file_t* iso_setup_install_(
    setup_t_*           setup,
    upd_file_t*         parent,
    const upd_driver_t* drv,
    const char*         name) {
  upd_file_t* f = upd_file_new(setup->iso, drv);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    return NULL;
  }
  ++setup->refcnt;
  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file = parent,
      .type = UPD_REQ_DIR_ADD,
      .dir  = { .entry = {
        .file = f,
        .name = (uint8_t*) name,
        .len  = utf8size_lazy(name),
      }, },
      .udata = setup,
      .cb    = iso_setup_install_cb_,
    });
  if (HEDLEY_UNLIKELY(!add)) {
    iso_setup_unref_(setup);
    upd_file_unref(f);
    return NULL;
  }
  return f;
}


static void iso_setup_lock_cb_(upd_file_lock_t* lock) {
  setup_t_*  setup = (void*) lock;
  upd_iso_t* iso   = setup->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    upd_iso_msgf(iso, "cannot setup iso since failure of acquiring lock\n");
    return;
  }

  assert(iso->files.n);
  upd_file_t* root = upd_file_get(iso, UPD_FILE_ID_ROOT);

  ++setup->refcnt;

  /* create system dirs */
  for (size_t i = 0; i < sizeof(root_dirs_)/sizeof(root_dirs_[0]); ++i) {
    upd_file_t* f =
      iso_setup_install_(setup, root, &upd_driver_dir, root_dirs_[i].name);
    if (HEDLEY_UNLIKELY(!f || f->id != root_dirs_[i].id)) {
      upd_iso_msgf(iso, "failed to create %s dir\n", root_dirs_[i].name);
      goto EXIT;
    }
  }

  /* put builtin programs in prog dir */
  upd_file_t* prog = upd_file_get(iso, UPD_FILE_ID_PROG);
# define install_(name) do {  \
    const bool ok = iso_setup_install_(  \
      setup, prog, &upd_driver_prog_##name, "upd."#name);  \
    if (HEDLEY_UNLIKELY(!ok)) {  \
      upd_iso_msgf(iso, "failed to install program, 'upd."#name"'\n");  \
      goto EXIT;  \
    }  \
  } while (0)
  install_(http);
  install_(parallelism);
# undef install_

EXIT:
  iso_setup_unref_(setup);
}
static void iso_setup_install_cb_(upd_req_t* req) {
  upd_file_unref(req->dir.entry.file);
  iso_setup_unref_(req->udata);
  upd_iso_unstack(req->file->iso, req);
}
