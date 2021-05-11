#include "common.h"


static
bool
iso_get_paths_(
  upd_iso_t* iso);

static
void
iso_create_dir_(
  upd_iso_t*  iso,
  const char* path);


static
void
iso_create_dir_cb_(
  upd_req_pathfind_t* pf);


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

  const bool ok =
    iso_get_paths_(iso) &&
    0 <= uv_loop_init(&iso->loop) &&
    0 <= uv_tty_init(&iso->loop, &iso->out, 1, 0);
  if (HEDLEY_UNLIKELY(!ok)) {
    return NULL;
  }

  upd_file_t* root = upd_file_new(iso, &upd_driver_dir);
  if (HEDLEY_UNLIKELY(root == NULL)) {
    return NULL;
  }
  assert(root->id == UPD_FILE_ID_ROOT);

  iso_create_dir_(iso, "/sys");
  iso_create_dir_(iso, "/var/srv");
  iso_create_dir_(iso, "/var/cli");

  upd_driver_setup(iso);
  return iso;
}

upd_iso_status_t upd_iso_run(upd_iso_t* iso) {
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  /* shutdown all sockets */
  upd_iso_close_all_conn(iso);
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  /* cleanup root directory */
  upd_file_unref(iso->files.p[0]);
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  /* close all system handlers */
  uv_close((uv_handle_t*) &iso->out, NULL);
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  /* make sure that all resources have been freed X) */
  assert(iso->stack.refcnt == 0);
  assert(iso->files.n      == 0);
  assert(iso->srv.n        == 0);
  assert(iso->cli.n        == 0);

  if (HEDLEY_UNLIKELY(0 > uv_loop_close(&iso->loop))) {
    return UPD_ISO_PANIC;
  }

  upd_array_clear(&iso->drivers);

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


static void iso_create_dir_(upd_iso_t* iso, const char* path) {
  const bool ok = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso    = iso,
      .path   = (uint8_t*) path,
      .len    = utf8size_lazy(path),
      .create = true,
      .cb     = iso_create_dir_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso,
      "allocation failure on pathfind request of '%s', while iso setup\n", path);
    return;
  }
}


static void iso_create_dir_cb_(upd_req_pathfind_t* pf) {
  upd_iso_t* iso = pf->base->iso;

  const upd_file_t* f = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(f == NULL)) {
    upd_iso_msgf(iso, "dir creation failure, while iso setup\n");
    return;
  }
}
