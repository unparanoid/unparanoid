#include "common.h"

#define LOG_PREFIX_ "upd.graphics.glfw.dev: "


static
bool
dev_init_(
  upd_file_t* f);

static
void
dev_deinit_(
  upd_file_t* f);

static
bool
dev_handle_(
  upd_req_t* req);

const upd_driver_t gra_glfw_dev = {
  .name   = (uint8_t*) "upd.graphics.glfw.dev",
  .cats   = (upd_req_cat_t[]) {0},
  .init   = dev_init_,
  .deinit = dev_deinit_,
  .handle = dev_handle_,
};


static
bool
thread_init_(
  upd_file_t* f);

static
void
thread_deinit_(
  upd_file_t* f);

static
bool
thread_handle_(
  upd_req_t* req);

static
void
thread_handle_async_(
  upd_file_t* f);

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
thread_errf_(
  upd_file_t* f,
  const char* fmt,
  ...);

static
void
thread_main_(
  void* udata);

static
bool
thread_handle_req_(
  upd_file_t*     f,
  gra_glfw_req_t* req);

static const upd_driver_t thread_ = {
  .name = (uint8_t*) "upd.graphics.glfw.dev.thread_",
  .cats = (upd_req_cat_t[]) {0},
  .init   = thread_init_,
  .deinit = thread_deinit_,
  .handle = thread_handle_,
};


static
void
thread_watch_cb_(
  upd_file_watch_t* w);


static atomic_flag glfw_busy_ = ATOMIC_FLAG_INIT;

static bool dev_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  if (HEDLEY_UNLIKELY(atomic_flag_test_and_set(&glfw_busy_))) {
    upd_iso_msgf(iso, LOG_PREFIX_"you cannot build two GLFW devices X(\n");
    return false;
  }

  upd_file_t* tf = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = &thread_,
    });
  if (HEDLEY_UNLIKELY(tf == NULL)) {
    atomic_flag_clear(&glfw_busy_);
    return false;
  }

  gra_glfw_dev_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    upd_file_unref(tf);
    upd_iso_msgf(iso, LOG_PREFIX_"context allocation failure\n");
    return false;
  }
  *ctx = (gra_glfw_dev_t) {
    .file   = f,
    .thread = tf,

    .watch = {
      .file  = tf,
      .udata = tf,
      .cb    = thread_watch_cb_,
    },

    .file_alive   = ATOMIC_VAR_INIT(true),
    .thread_alive = ATOMIC_VAR_INIT(true),

    .done = ATOMIC_VAR_INIT(true),
  };
  f->ctx  = ctx;
  tf->ctx = ctx;

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    upd_file_unref(tf);
    upd_iso_msgf(iso, LOG_PREFIX_"self watch failure\n");
    return false;
  }

  if (HEDLEY_UNLIKELY(!upd_iso_start_thread(iso, thread_main_, tf))) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    atomic_flag_clear(&glfw_busy_);
    upd_iso_msgf(iso, LOG_PREFIX_"failed to start GLFW thread\n");
    return false;
  }
  return true;
}

static void dev_deinit_(upd_file_t* f) {
  gra_glfw_dev_t* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx)) {
    atomic_store(&ctx->file_alive, false);
    glfwPostEmptyEvent();
    ctx->file = NULL;
  }
  /* resources are freed by thread_deinit_ */
}

static bool dev_handle_(upd_req_t* req) {
  (void) req;
  return false;
}


static bool thread_init_(upd_file_t* f) {
  (void) f;
  return true;
}

static void thread_deinit_(upd_file_t* f) {
  gra_glfw_dev_t* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->file)) {
    ctx->file->ctx = NULL;
  }
  upd_file_unwatch(&ctx->watch);

  atomic_flag_clear(&glfw_busy_);
  upd_free(&ctx);
}

static bool thread_handle_(upd_req_t* req) {
  (void) req;
  return false;
}

static void thread_handle_async_(upd_file_t* f) {
  upd_iso_t*      iso = f->iso;
  gra_glfw_dev_t* ctx = f->ctx;

  const bool done = atomic_load(&ctx->done);
  if (HEDLEY_LIKELY(done && ctx->req)) {
    gra_glfw_req_t* req = ctx->req;
    ctx->req = NULL;

    if (HEDLEY_UNLIKELY(ctx->error[0])) {
      upd_iso_msgf(iso, LOG_PREFIX_"GLFW thread error: %s\n", ctx->error);
      ctx->error[0] = 0;
    }
    req->cb(req);
  }
  if (HEDLEY_UNLIKELY(!atomic_load(&ctx->thread_alive))) {
    if (HEDLEY_UNLIKELY(!done && ctx->req)) {
      gra_glfw_req_t* req = ctx->req;
      req->ok = false;
      req->cb(req);
    }
    upd_iso_msgf(iso, LOG_PREFIX_"GLFW thread exited\n");
    upd_file_unref(f);
  }
}

static void thread_errf_(upd_file_t* f, const char* fmt, ...) {
  gra_glfw_dev_t* ctx = f->ctx;

  va_list args;
  va_start(args, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, args);
  va_end(args);
}

static void thread_main_(void* udata) {
  upd_file_t*     f   = udata;
  upd_iso_t*      iso = f->iso;
  gra_glfw_dev_t* ctx = f->ctx;

  const upd_file_id_t id = f->id;

  if (HEDLEY_UNLIKELY(glfwInit() == 0)) {
    thread_errf_(f, "glfwInit failure");
    goto EXIT;
  }

  /* clears error (glfwPostEvent may be called before glfwInit) */
  glfwGetError(NULL);

  while (atomic_load(&ctx->file_alive)) {
    if (HEDLEY_UNLIKELY(!atomic_load(&ctx->done))) {
      ctx->req->ok = thread_handle_req_(f, ctx->req);
      atomic_store(&ctx->done, true);

      if (HEDLEY_UNLIKELY(!upd_file_trigger_async(iso, id))) {
        fprintf(stderr, LOG_PREFIX_
          "failed to trigger ASYNC event, this may cause dead lock X(\n");
      }
    }
    glfwWaitEvents();
  }
  glfwTerminate();

EXIT:
  atomic_store(&ctx->thread_alive, false);
  if (HEDLEY_UNLIKELY(!upd_file_trigger_async(iso, id))) {
    fprintf(stderr, LOG_PREFIX_
      "failed to trigger ASYNC event, "
      "this may cause iso machine stuck X(\n");
  }
}

static bool thread_handle_req_(upd_file_t* f, gra_glfw_req_t* req) {
  switch (req->type) {
  case GRA_GLFW_REQ_GL3_INIT:
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

    req->win = glfwCreateWindow(16, 16, "OpenGL context", NULL, NULL);
    if (HEDLEY_UNLIKELY(req->win == NULL)) {
      thread_errf_(f, "failed to create OpenGL context");
      return false;
    }

    glfwMakeContextCurrent(req->win);
    const char* err;
    if (HEDLEY_UNLIKELY(glfwGetError(&err))) {
      thread_errf_(f, "glfwMakeContextCurrent error: %s", err);
      return false;
    }
    if (HEDLEY_UNLIKELY(glewInit())) {
      thread_errf_(f, "glewInit error");
      return false;
    }
    glfwMakeContextCurrent(NULL);
    return true;

  case GRA_GLFW_REQ_SUB_INIT: {
    glfwWindowHint(GLFW_VISIBLE,   GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow* win =
      glfwCreateWindow(100, 100, "subwindow", NULL, req->sub.share);
    if (HEDLEY_UNLIKELY(win == NULL)) {
      thread_errf_(f, "failed to create OpenGL window");
      return false;
    }

    const gra_glfw_win_cb_t* cb = req->sub.cb;
    if (cb) {
      glfwSetWindowPosCallback(win, cb->pos);
      glfwSetWindowSizeCallback(win, cb->size);
      glfwSetWindowCloseCallback(win, cb->close);
      glfwSetWindowRefreshCallback(win, cb->refresh);
      glfwSetWindowFocusCallback(win, cb->focus);
      glfwSetWindowIconifyCallback(win, cb->iconify);
      glfwSetWindowMaximizeCallback(win, cb->maximize);
      glfwSetFramebufferSizeCallback(win, cb->framebuffersize);
      glfwSetWindowContentScaleCallback(win, cb->contentscale);
    }
    req->sub.win = win;
  } return true;

  case GRA_GLFW_REQ_WIN_DEINIT:
    glfwDestroyWindow(req->win);
    return true;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }
}


static void thread_watch_cb_(upd_file_watch_t* w) {
  upd_file_t* f = w->udata;

  switch (w->event) {
  case UPD_FILE_ASYNC:
    thread_handle_async_(f);
    return;

  default:
    return;
  }
}
