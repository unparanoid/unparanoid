#include "common.h"


#define SHUTDOWN_TIMER_ 1000

#define WALKER_PERIOD_           1000
#define WALKER_FILES_PER_PERIOD_ 1000


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

static
void
iso_signal_cb_(
  uv_signal_t* sig,
  int          signum);

static
void
iso_shutdown_timer_cb_(
  uv_timer_t* timer);


static
void
iso_walker_handle_(
  upd_file_t* f);

static
void
iso_walker_cb_(
  uv_timer_t* timer);


upd_iso_t* upd_iso_new(size_t stacksz) {
  /*  It's unnecessary to clean up resources in aborted,
   * because app exits immediately. */

  upd_iso_t* iso = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&iso, sizeof(*iso)+stacksz))) {
    return NULL;
  }
  *iso = (upd_iso_t) {
    .sigint         = { .data = iso, },
    .sighup         = { .data = iso, },
    .shutdown_timer = { .data = iso, },

    .stack = {
      .size = stacksz,
      .ptr  = (uint8_t*) (iso+1),
    },
    .walker = {
      .timer = { .data = iso, },
    },
  };

  const bool ok =
    iso_get_paths_(iso) &&
    0 <= uv_loop_init(&iso->loop) &&
    0 <= uv_tty_init(&iso->loop, &iso->out, 1, 0) &&
    0 <= uv_signal_init(&iso->loop, &iso->sigint) &&
    0 <= uv_signal_init(&iso->loop, &iso->sighup) &&
    0 <= uv_timer_init(&iso->loop, &iso->walker.timer) &&
    0 <= uv_timer_init(&iso->loop, &iso->shutdown_timer) &&
    0 <= uv_signal_start(&iso->sigint, iso_signal_cb_, SIGINT) &&
    0 <= uv_signal_start(&iso->sighup, iso_signal_cb_, SIGHUP) &&
    0 <= uv_timer_start(
      &iso->walker.timer, iso_walker_cb_, WALKER_PERIOD_, WALKER_PERIOD_);
  if (HEDLEY_UNLIKELY(!ok)) {
    return NULL;
  }
  uv_unref((uv_handle_t*) &iso->sigint);
  uv_unref((uv_handle_t*) &iso->sighup);
  uv_unref((uv_handle_t*) &iso->walker.timer);

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
  uv_close((uv_handle_t*) &iso->out,            NULL);
  uv_close((uv_handle_t*) &iso->sigint,         NULL);
  uv_close((uv_handle_t*) &iso->sighup,         NULL);
  uv_close((uv_handle_t*) &iso->shutdown_timer, NULL);
  uv_close((uv_handle_t*) &iso->walker.timer,   NULL);
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  /* make sure that all resources have been freed X) */
  assert(iso->stack.refcnt == 0);
  assert(iso->files.n      == 0);
  assert(iso->srv.n        == 0);
  assert(iso->cli.n        == 0);

  /* unload all dynamic libraries */
  for (size_t i = 0; i < iso->libs.n; ++i) {
    uv_lib_t* lib = iso->libs.p[i];
    uv_dlclose(lib);
    upd_free(&lib);
  }
  upd_array_clear(&iso->libs);

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
  uv_signal_stop(&iso->sigint);
  uv_signal_stop(&iso->sighup);
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

static void iso_signal_cb_(uv_signal_t* sig, int signum) {
  upd_iso_t* iso = sig->data;

  switch (signum) {
  case SIGINT:
    if (uv_is_active((uv_handle_t*) &iso->shutdown_timer)) {
      upd_iso_msgf(iso, "rebooting...\n");
      uv_timer_stop(&iso->shutdown_timer);
      upd_iso_exit(iso, UPD_ISO_REBOOT);
    } else {
      upd_iso_msgf(iso,
        "SIGINT received: "
        "wait for 1 sec to shutdown, or press Ctrl+C again to reboot\n");
      uv_timer_start(
        &iso->shutdown_timer, iso_shutdown_timer_cb_, SHUTDOWN_TIMER_, 0);
    }
    break;
  case SIGHUP:
    upd_iso_msgf(iso, "SIGHUP received: exiting...\n");
    upd_iso_exit(iso, UPD_ISO_SHUTDOWN);
    break;
  }
}

static void iso_shutdown_timer_cb_(uv_timer_t* timer) {
  upd_iso_t* iso = timer->data;
  upd_iso_msgf(iso, "shutting down...\n");
  upd_iso_exit(iso, UPD_ISO_SHUTDOWN);
}


static void iso_walker_handle_(upd_file_t* f) {
  upd_file_t_* f_ = (void*) f;

  const uint64_t now = upd_iso_now(f->iso);

  /* lock timeout handling */
  for (size_t i = 0; i < f_->lock.pending.n;) {
    upd_file_lock_t* k = f_->lock.pending.p[i];

    const uint64_t thresh = k->basetime + k->timeout;
    if (HEDLEY_UNLIKELY(now >= thresh)) {
      upd_file_unlock(k);
      continue;
    }
    ++i;
  }

  /* trigger uncache event */
  const bool uncache =
    f->driver->uncache_period && f->last_req &&
    f->last_req >= f->last_uncache;
  if (HEDLEY_UNLIKELY(uncache)) {
    const uint64_t t = now > f->last_req? now - f->last_req: 0;
    if (HEDLEY_UNLIKELY(t > f->driver->uncache_period)) {
      upd_file_trigger(f, UPD_FILE_UNCACHE);
      f->last_uncache = now;
    }
  }
}

static void iso_walker_cb_(uv_timer_t* timer) {
  upd_iso_t*   iso   = timer->data;
  upd_file_t** files = (void*) iso->files.p;

  if (HEDLEY_UNLIKELY(iso->files.n == 0)) {
    return;
  }

  const upd_file_id_t needle = iso->walker.last_seen;

  ssize_t i = 0, l = 0, r = (ssize_t) iso->files.n - 1;
  while (l < r) {
    i = (l+r)/2;
    if (HEDLEY_UNLIKELY(files[i]->id == needle)) {
      break;
    }
    if (files[i]->id > needle) {
      r = i-1;
    } else {
      l = i+1;
    }
  }
  if (files[i]->id <= needle) {
    ++i;
  }

  for (size_t j = 0; j < iso->files.n && j < WALKER_FILES_PER_PERIOD_; ++j) {
    if (HEDLEY_UNLIKELY((size_t) i >= iso->files.n)) {
      i = 0;
    }
    upd_file_t* f = iso->files.p[i++];
    iso_walker_handle_(f);
    iso->walker.last_seen = f->id;
  }
}
