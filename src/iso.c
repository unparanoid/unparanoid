#include "common.h"


#define SHUTDOWN_TIMER_ 1000

#define DESTROYER_PERIOD_ 100

#define WALKER_PERIOD_               1000
#define WALKER_FILES_PER_PERIOD_     1000
#define WALKER_FILES_UNCACHE_DELAY_ 10000


typedef struct curl_t_ {
  upd_iso_t* iso;

  void*             udata;
  upd_iso_curl_cb_t cb;
} curl_t_;

typedef struct curl_sock_t_ {
  upd_iso_t* iso;

  uv_poll_t     poll;
  curl_socket_t fd;
} curl_sock_t_;


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
iso_abort_all_pkg_installations_(
  upd_iso_t* iso);


static
void
walker_handle_(
  upd_file_t* f);


static
void
curl_check_(
  upd_iso_t* iso);


static
void
iso_create_dir_cb_(
  upd_pathfind_t* pf);

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
destroyer_cb_(
  uv_timer_t* timer);


static
void
walker_cb_(
  uv_timer_t* timer);


static
int
curl_socket_cb_(
  CURL*         curl,
  curl_socket_t s,
  int           act,
  void*         udata,
  void*         sockp);

static
void
curl_start_timer_cb_(
  CURLM* curl,
  long   ms,
  void*  udata);

static
void
curl_timer_cb_(
  uv_timer_t* timer);

static
void
curl_poll_cb_(
  uv_poll_t* poll,
  int        status,
  int        event);

static
void
curl_sock_close_cb_(
  uv_handle_t* handle);


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
    .destroyer      = { .data = iso, },

    .status = UPD_ISO_RUNNING,

    .stack = {
      .size = stacksz,
      .ptr  = (uint8_t*) (iso+1),
    },
    .walker = {
      .timer = { .data = iso, },
    },
    .curl = {
      .timer = { .data = iso, },
    },
  };

  /* init uv handles */
  const bool uv_ok =
    iso_get_paths_(iso) &&
    0 <= uv_loop_init(&iso->loop) &&
    0 <= uv_tty_init(&iso->loop, &iso->out, 1, 0) &&
    0 <= uv_signal_init(&iso->loop, &iso->sigint) &&
    0 <= uv_signal_init(&iso->loop, &iso->sighup) &&
    0 <= uv_timer_init(&iso->loop, &iso->walker.timer) &&
    0 <= uv_timer_init(&iso->loop, &iso->shutdown_timer) &&
    0 <= uv_timer_init(&iso->loop, &iso->destroyer) &&
    0 <= uv_signal_start(&iso->sigint, iso_signal_cb_, SIGINT) &&
    0 <= uv_signal_start(&iso->sighup, iso_signal_cb_, SIGHUP) &&
    0 <= uv_timer_start(
      &iso->walker.timer, walker_cb_, WALKER_PERIOD_, WALKER_PERIOD_) &&
    0 <= uv_mutex_init(&iso->mtx);
  if (HEDLEY_UNLIKELY(!uv_ok)) {
    return NULL;
  }
  uv_unref((uv_handle_t*) &iso->walker.timer);

  /* init curl */
  iso->curl.ctx = curl_multi_init();
  if (HEDLEY_UNLIKELY(iso->curl.ctx == NULL)) {
    return NULL;
  }
  const bool curl_ok =
    0 <= uv_timer_init(&iso->loop, &iso->curl.timer) &&
    !curl_multi_setopt(
      iso->curl.ctx, CURLMOPT_SOCKETFUNCTION, curl_socket_cb_) &&
    !curl_multi_setopt(iso->curl.ctx, CURLMOPT_SOCKETDATA, iso) &&
    !curl_multi_setopt(
      iso->curl.ctx, CURLMOPT_TIMERFUNCTION, curl_start_timer_cb_) &&
    !curl_multi_setopt(iso->curl.ctx, CURLMOPT_TIMERDATA, iso);
  if (HEDLEY_UNLIKELY(!curl_ok)) {
    return NULL;
  }

  /* init filesystem */
  upd_file_t* root = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = &upd_driver_dir,
    });
  if (HEDLEY_UNLIKELY(root == NULL)) {
    return NULL;
  }
  assert(root->id == UPD_FILE_ID_ROOT);

  iso_create_dir_(iso, "/sys");

  upd_driver_setup(iso);
  return iso;
}

upd_iso_status_t upd_iso_run(upd_iso_t* iso) {
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  iso_abort_all_pkg_installations_(iso);

  /* disalbe signal handler */
  uv_signal_stop(&iso->sigint);
  uv_signal_stop(&iso->sighup);

  /* trigger shutdown event */
  upd_file_unref(iso->files.p[0]);
  for (size_t i = iso->files.n; ;) {
    if (HEDLEY_UNLIKELY(i > iso->files.n)) {
      i = iso->files.n;
    }
    if (HEDLEY_UNLIKELY(i == 0)) {
      break;
    }
    upd_file_t* f = iso->files.p[--i];
    upd_file_trigger(f, UPD_FILE_SHUTDOWN);
  }

  /* start destroyer */
  const int destroy = uv_timer_start(
    &iso->destroyer, destroyer_cb_, 0, DESTROYER_PERIOD_);
  if (HEDLEY_UNLIKELY(0 > destroy)) {
    return UPD_ISO_PANIC;
  }
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  /* close all system handlers */
  uv_close((uv_handle_t*) &iso->out,            NULL);
  uv_close((uv_handle_t*) &iso->sigint,         NULL);
  uv_close((uv_handle_t*) &iso->sighup,         NULL);
  uv_close((uv_handle_t*) &iso->shutdown_timer, NULL);
  uv_close((uv_handle_t*) &iso->destroyer,      NULL);
  uv_close((uv_handle_t*) &iso->walker.timer,   NULL);
  uv_close((uv_handle_t*) &iso->curl.timer,     NULL);
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }
  assert(iso->stack.refcnt == 0);
  assert(iso->files.n      == 0);
  assert(iso->threads.n    == 0);

  uv_mutex_destroy(&iso->mtx);

  /* forget all packages */
  for (size_t i = 0; i < iso->pkgs.n; ++i) {
    upd_pkg_t* pkg = iso->pkgs.p[i];
    assert(!pkg->install);
    upd_free(&pkg);
  }
  upd_array_clear(&iso->pkgs);

  /* cleanup curl */
  curl_multi_cleanup(iso->curl.ctx);

  /* unload all dynamic libraries */
  for (size_t i = 0; i < iso->libs.n; ++i) {
    uv_lib_t* lib = iso->libs.p[i];
    uv_dlclose(lib);
    upd_free(&lib);
  }
  upd_array_clear(&iso->libs);

  /* closes loop */
  if (HEDLEY_UNLIKELY(0 > uv_loop_close(&iso->loop))) {
    return UPD_ISO_PANIC;
  }

  /* forget all drivers */
  upd_array_clear(&iso->drivers);

  const upd_iso_status_t ret = iso->status;
  upd_free(&iso);
  return ret;
}

void upd_iso_exit(upd_iso_t* iso, upd_iso_status_t status) {
  if (HEDLEY_UNLIKELY(iso->teardown)) {
    return;
  }
  iso->status   = status;
  iso->teardown = true;
  uv_stop(&iso->loop);
}


bool upd_iso_curl_perform(
    upd_iso_t* iso, CURL* curl, upd_iso_curl_cb_t cb, void* udata) {
  curl_t_* ctx = upd_iso_stack(iso, sizeof(*ctx));
  if (HEDLEY_UNLIKELY(ctx == NULL)) {
    return false;
  }
  *ctx = (curl_t_) {
    .iso   = iso,
    .udata = udata,
    .cb    = cb,
  };
  const bool ok =
    !curl_easy_setopt(curl, CURLOPT_PRIVATE, ctx) &&
    !curl_multi_add_handle(iso->curl.ctx, curl);
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_unstack(iso, ctx);
    return false;
  }
  return true;
}


static bool iso_get_paths_(upd_iso_t* iso) {
  uint8_t cwd[UPD_PATH_MAX];
  size_t  cwdlen = UPD_PATH_MAX;
  if (HEDLEY_UNLIKELY(0 > uv_cwd((char*) cwd, &cwdlen))) {
    return false;
  }

  const char* env = getenv("UPD_RUNTIME_PATH");
  if (HEDLEY_UNLIKELY(env && env[0])) {
    const size_t len = cwk_path_get_absolute(
      (char*) cwd, env, (char*) iso->path.runtime, UPD_PATH_MAX);
    if (HEDLEY_UNLIKELY(len >= UPD_PATH_MAX)) {
      return false;
    }

  } else {
    size_t len = UPD_PATH_MAX;
    const bool exepath = 0 <= uv_exepath((char*) iso->path.runtime, &len);
    if (HEDLEY_UNLIKELY(!exepath || len >= UPD_PATH_MAX)) {
      return false;
    }
    cwk_path_get_dirname((char*) iso->path.runtime, &len);
    iso->path.runtime[len] = 0;
  }
  cwk_path_normalize(
    (char*) iso->path.runtime, (char*) iso->path.runtime, UPD_PATH_MAX);

  const size_t pkgpath = cwk_path_join(
    (char*) iso->path.runtime, "pkg", (char*) iso->path.pkg, UPD_PATH_MAX);
  if (HEDLEY_UNLIKELY(pkgpath >= UPD_PATH_MAX)) {
    return false;
  }

  env = getenv("UPD_WORKING_PATH");
  if (HEDLEY_UNLIKELY(env && env[0])) {
    const size_t len = cwk_path_get_absolute(
      (char*) cwd, env, (char*) iso->path.working, UPD_PATH_MAX);
    if (HEDLEY_UNLIKELY(len >= UPD_PATH_MAX)) {
      return false;
    }
  } else {
    utf8cpy(iso->path.working, cwd);
  }
  cwk_path_normalize(
    (char*) iso->path.working, (char*) iso->path.working, UPD_PATH_MAX);
  return true;
}


static void iso_create_dir_(upd_iso_t* iso, const char* path) {
  const bool ok = upd_pathfind_with_dup(&(upd_pathfind_t) {
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

static void iso_abort_all_pkg_installations_(upd_iso_t* iso) {
  for (size_t i = 0; i < iso->pkgs.n; ++i) {
    upd_pkg_t* pkg = iso->pkgs.p[i];
    if (HEDLEY_UNLIKELY(pkg->install)) {
      upd_pkg_abort_install(pkg->install);
    }
  }
}


static void walker_handle_(upd_file_t* f) {
  upd_file_t_* f_  = (void*) f;
  upd_iso_t*   iso = f->iso;

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
    f_->lock.refcnt == 0 &&
    f->cache > 0         &&
    f->cache >= iso->walker.cache.thresh;
  if (HEDLEY_UNLIKELY(uncache)) {
    const uint64_t thresh = f->last_touch + WALKER_FILES_UNCACHE_DELAY_;
    if (HEDLEY_UNLIKELY(now >= thresh)) {
      upd_file_trigger(f, UPD_FILE_UNCACHE);
    }
  }
}


static void curl_check_(upd_iso_t* iso) {
  CURLM* multi = iso->curl.ctx;

  CURLMsg* msg;
  int      pending;
  while ((msg = curl_multi_info_read(multi, &pending))) {
    if (msg->msg == CURLMSG_DONE) {
      CURL* curl = msg->easy_handle;

      curl_t_* ctx;
      const bool getinfo =
        !curl_easy_getinfo(curl, CURLINFO_PRIVATE, (void*) &ctx);
      if (HEDLEY_UNLIKELY(!getinfo)) {
        continue;
      }
      curl_multi_remove_handle(multi, curl);
      ctx->cb(curl, msg->data.result, ctx->udata);
      upd_iso_unstack(ctx->iso, ctx);
    }
  }
}


static void iso_create_dir_cb_(upd_pathfind_t* pf) {
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


static void destroyer_cb_(uv_timer_t* timer) {
  upd_iso_t* iso = timer->data;

  iso_abort_all_pkg_installations_(iso);

  if (HEDLEY_UNLIKELY(iso->files.n == 0)) {
    uv_timer_stop(&iso->destroyer);
  }
}


static void walker_cb_(uv_timer_t* timer) {
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

      const size_t cache = iso->walker.cache.part;
      const size_t avg   = cache / iso->files.n;
      const size_t pth   = iso->walker.cache.thresh;

      iso->walker.cache.whole  = cache;
      iso->walker.cache.avg    = avg;
      iso->walker.cache.thresh = pth/10*2 + avg/10*6;
      iso->walker.cache.part   = 0;
    }
    upd_file_t* f = iso->files.p[i++];
    walker_handle_(f);

    iso->walker.cache.part += f->cache;
    iso->walker.last_seen   = f->id;
  }
}


static int curl_socket_cb_(
    CURL* curl, curl_socket_t s, int act, void* userp, void* sockp) {
  upd_iso_t*    iso   = userp;
  curl_sock_t_* sock  = sockp;
  CURLM*        multi = iso->curl.ctx;

  curl_t_* ctx;
  curl_easy_getinfo(curl, CURLINFO_PRIVATE, &ctx);

  switch (act) {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT: {
    if (HEDLEY_UNLIKELY(sock == NULL)) {
      sock = upd_iso_stack(iso, sizeof(*sock));
      if (HEDLEY_UNLIKELY(sock == NULL)) {
        upd_iso_msgf(iso, "curl: socket context allocation failure\n");
        return 1;
      }
      *sock = (curl_sock_t_) {
        .iso  = ctx->iso,
        .fd   = s,
        .poll = { .data = sock, },
      };
      const int init = uv_poll_init_socket(&iso->loop, &sock->poll, s);
      if (HEDLEY_UNLIKELY(0 > init)) {
        upd_iso_unstack(iso, sock);
        upd_iso_msgf(iso, "curl: poll init failure\n");
        return 1;
      }
    }
    curl_multi_assign(multi, s, sock);

    int ev = 0;
    if (act != CURL_POLL_OUT) ev |= UV_READABLE;
    if (act != CURL_POLL_IN)  ev |= UV_WRITABLE;
    uv_poll_start(&sock->poll, ev, curl_poll_cb_);
  } break;

  case CURL_POLL_REMOVE:
    if (HEDLEY_LIKELY(sock)) {   
      uv_poll_stop(&sock->poll);
      uv_close((uv_handle_t*) &sock->poll, curl_sock_close_cb_);
      curl_multi_assign(multi, s, NULL);
    }
    break;
  }
  return 0;
}

static void curl_start_timer_cb_(CURLM* multi, long ms, void* udata) {
  upd_iso_t* iso = udata;
  (void) multi;

  if (ms < 0) ms = 1;
  uv_timer_start(&iso->curl.timer, curl_timer_cb_, ms, 0);
}

static void curl_timer_cb_(uv_timer_t* timer) {
  upd_iso_t* iso = timer->data;

  int temp;
  curl_multi_socket_action(iso->curl.ctx, CURL_SOCKET_TIMEOUT, 0, &temp);
  curl_check_(iso);
}

static void curl_poll_cb_(uv_poll_t* poll, int status, int event) {
  curl_sock_t_* sock = poll->data;
  upd_iso_t*    iso  = sock->iso;

  if (HEDLEY_UNLIKELY(status < 0)) {
    upd_iso_msgf(iso, "curl: poll failure\n");
  }

  int flags = 0;
  if (event & UV_READABLE) flags |= CURL_CSELECT_IN;
  if (event & UV_WRITABLE) flags |= CURL_CSELECT_OUT;

  int temp;
  curl_multi_socket_action(iso->curl.ctx, sock->fd, flags, &temp);
  curl_check_(iso);
}

static void curl_sock_close_cb_(uv_handle_t* handle) {
  curl_sock_t_* sock = handle->data;
  upd_iso_unstack(sock->iso, sock);
}
