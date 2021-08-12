#pragma once

#define GRA_GLFW_DEV_PATH "/sys/upd.graphics.glfw.dev"

extern const upd_driver_t gra_glfw_dev;


typedef struct gra_glfw_dev_t gra_glfw_dev_t;
typedef struct gra_glfw_req_t gra_glfw_req_t;


struct gra_glfw_dev_t {
  upd_file_t* file;
  upd_file_t* thread;

  upd_file_watch_t watch;

  atomic_bool thread_alive;
  atomic_bool file_alive;

  atomic_bool     done;
  gra_glfw_req_t* req;

  char error[256];
};


typedef enum gra_glfw_req_type_t {
  GRA_GLFW_REQ_GL3_INIT,
  GRA_GLFW_REQ_GL3_DEINIT,
} gra_glfw_req_type_t;

struct gra_glfw_req_t {
  upd_file_t*     dev;
  upd_file_lock_t lock;

  gra_glfw_req_type_t type;

  GLFWwindow* win;

  unsigned ok : 1;

  void* udata;
  void
  (*cb)(
    gra_glfw_req_t* req);
};


HEDLEY_NON_NULL(1)
static inline
void
gra_glfw_req(
  gra_glfw_req_t* req);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
bool
gra_glfw_lock_and_req(
  gra_glfw_req_t* req);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
gra_glfw_req_t*
gra_glfw_lock_and_req_with_dup(
  const gra_glfw_req_t* src);


static
void
gra_glfw_lock_and_req_lock_cb_(
  upd_file_lock_t* k);


static inline void gra_glfw_req(gra_glfw_req_t* req) {
  upd_file_t*     f   = req->dev;
  gra_glfw_dev_t* ctx = f->ctx;

  assert(atomic_load(&ctx->done));

  ctx->req = req;
  atomic_store(&ctx->done, false);
}

static inline bool gra_glfw_lock_and_req(gra_glfw_req_t* req) {
  upd_file_t* f = req->dev;

  req->lock = (upd_file_lock_t) {
    .file  = f,
    .ex    = true,
    .udata = req,
    .cb    = gra_glfw_lock_and_req_lock_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&req->lock))) {
    return false;
  }
  return true;
}

static inline gra_glfw_req_t* gra_glfw_lock_and_req_with_dup(
    const gra_glfw_req_t* src) {
  upd_file_t* f   = src->dev;
  upd_iso_t*  iso = f->iso;

  gra_glfw_req_t* req = upd_iso_stack(iso, sizeof(*req));
  if (HEDLEY_UNLIKELY(req == NULL)) {
    return NULL;
  }
  *req = *src;

  if (HEDLEY_UNLIKELY(!gra_glfw_lock_and_req(req))) {
    upd_iso_unstack(src->dev->iso, req);
    return NULL;
  }
  return req;
}


static void gra_glfw_lock_and_req_lock_cb_(upd_file_lock_t* k) {
  gra_glfw_req_t* req = k->udata;
  upd_file_t*     f   = req->dev;
  upd_iso_t*      iso = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_iso_msgf(iso, "GLFW device lock cancelled in lock-and-req\n");
    req->ok = false;
    req->cb(req);
    return;
  }
  gra_glfw_req(req);
}
