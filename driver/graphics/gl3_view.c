#include "common.h"

#define LOG_PREFIX_ "upd.graphics.gl3.view: "

#define VIEW_QUEUE_DURATION_ 10000  /* 10 secs */
#define VIEW_QUEUE_MAX_         64


#include "gl3_view.vsh.h"
#include "gl3_view.fsh.h"

#define SHADER_UNI_TEX_   0
#define SHADER_UNI_SCALE_ 1
#define SHADER_UNI_SIZE_  2


typedef struct view_t_   view_t_;
typedef struct stream_t_ stream_t_;
typedef struct frame_t_  frame_t_;


struct view_t_ {
  upd_file_t* gl;
  upd_file_t* glfw;
  upd_file_t* th;

  upd_file_watch_t watch;
  upd_file_lock_t  gl_lock;

  atomic_uintptr_t stream;  /* pointer to stream file */

  size_t refcnt;

  GLuint vao;
  GLuint prog;
  GLuint sampler;
  GLuint fbo;
  GLuint tex;

  atomic_bool thread_alive;
  atomic_bool file_alive;

  unsigned broken : 1;

  struct {
    GLFWwindow* ptr;
    atomic_bool dirty;
    atomic_uint_least32_t w, h;

    uint32_t tw, th;
    double   aspect;
  } win;

  upd_array_of(frame_t_*) done;

  uint8_t err[256];
};

static
bool
view_init_(
  upd_file_t* f);

static
void
view_deinit_(
  upd_file_t* f);

static
bool
view_handle_(
  upd_req_t* req);

static
void
view_req_update_(
  upd_file_t* f);

static
void
view_teardown_gl_(
  void* udata);

static
void
view_size_cb_(
  GLFWwindow* win,
  int         w,
  int         h);

static
void
view_refresh_cb_(
  GLFWwindow* w);

static
void
view_teardown_gl_cb_(
  upd_iso_t* iso,
  void*      udata);

const upd_driver_t gra_gl3_view = {
  .name = (uint8_t*) "upd.graphics.gl3.view",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_DSTREAM,
    0,
  },
  .init   = view_init_,
  .deinit = view_deinit_,
  .handle = view_handle_,
};

static const gra_glfw_win_cb_t win_cb_ = {
  .size            = view_size_cb_,
  .framebuffersize = view_size_cb_,
  .refresh         = view_refresh_cb_,
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
thread_main_(
  void* udata);

static
void
thread_update_fb_(
  upd_file_t* f,
  upd_file_t* stf);

static
void
thread_watch_cb_(
  upd_file_watch_t* w);

static
void
thread_teardown_glfw_cb_(
  gra_glfw_req_t* req);

static const upd_driver_t thread_ = {
  .name   = (uint8_t*) "upd.graphics.gl3.view.thread_",
  .cats   = (upd_req_cat_t[]) {0},
  .flags  = {
    .mutex = true,
  },
  .init   = thread_init_,
  .deinit = thread_deinit_,
  .handle = thread_handle_,
};


typedef struct stream_t_ {
  upd_file_t* target;

  upd_file_watch_t watch;

  uint64_t ut_den;
  uint64_t ut_num;
  uint64_t tbase;

  upd_msgpack_t     mpk;
  msgpack_unpacked  upkd;
  upd_proto_parse_t par;

  unsigned init : 1;

  upd_array_of(frame_t_*) waiting;
  atomic_uintptr_t        pending;

  struct {
    double   aspect;
    uint32_t tw, th;
  } view;
} stream_t_;

static
bool
stream_init_(
  upd_file_t* f);

static
void
stream_deinit_(
  upd_file_t* f);

static
bool
stream_handle_(
  upd_req_t* req);

static
void
stream_return_(
  upd_file_t* f,
  bool        ok,
  const char* msg);

static
void
stream_msgpack_cb_(
  upd_msgpack_t* mpk);

static
void
stream_proto_parse_cb_(
  upd_proto_parse_t* par);

static
void
stream_watch_cb_(
  upd_file_watch_t* w);

static const upd_driver_t stream_ = {
  .name   = (uint8_t*) "upd.graphics.gl3.view.stream_",
  .cats   = (upd_req_cat_t[]) {0},
  .flags  = {
    .timer = true,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


typedef struct init_t_ {
  upd_file_t* file;

  upd_file_lock_t self_lock;
  upd_file_lock_t gl_lock;

  gra_glfw_req_t glfw_req;

  size_t refcnt;

  unsigned self_locked : 1;
  unsigned gl_locked   : 1;
  unsigned glfw_locked : 1;

  uint8_t err[256];
} init_t_;

static
void
init_setup_gl_(
  void* udata);

static
void
init_unref_(
  init_t_* ini);

static
void
init_lock_self_cb_(
  upd_file_lock_t* k);

static
void
init_pathfind_gl_cb_(
  upd_pathfind_t* pf);

static
void
init_lock_gl_cb_(
  upd_file_lock_t* k);

static
void
init_win_create_cb_(
  gra_glfw_req_t* req);

static
void
init_setup_gl_cb_(
  upd_iso_t* iso,
  void*      udata);


struct frame_t_ {
  upd_file_t* stream;
  upd_file_t* tensor;
  uint64_t    time;

  uint32_t w, h;  /* image size */

  GLuint tex;

  uint8_t*          data;
  uint8_t           ch;
  upd_tensor_type_t type;

  unsigned locked  : 1;
  unsigned fetched : 1;

  upd_file_lock_t lock;
  upd_req_t       req;
};

static
void
frame_delete_(
  frame_t_* frame);

static
void
frame_lock_tensor_cb_(
  upd_file_lock_t* k);

static
void
frame_fetch_tensor_cb_(
  upd_req_t* req);

static
void
frame_flush_tensor_cb_(
  upd_req_t* req);


static bool view_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  view_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    upd_iso_msgf(iso, LOG_PREFIX_"context allocation failure\n");
    return false;
  }
  *ctx = (view_t_) {
    .thread_alive = ATOMIC_VAR_INIT(true),
    .file_alive   = ATOMIC_VAR_INIT(true),
    .win = {
      .dirty = ATOMIC_VAR_INIT(false),
      .w     = ATOMIC_VAR_INIT(16),
      .h     = ATOMIC_VAR_INIT(16),
    },
  };
  f->ctx = ctx;

  upd_file_t* th = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = &thread_,
    });
  if (HEDLEY_UNLIKELY(th == NULL)) {
    upd_free(&ctx);
    upd_iso_msgf(iso, LOG_PREFIX_"thread file creation failure\n");
    return false;
  }
  ctx->th = th;
  th->ctx = ctx;

  init_t_* ini = upd_iso_stack(iso, sizeof(*ini));
  if (HEDLEY_UNLIKELY(ini == NULL)) {
    upd_file_unref(th);
    upd_iso_msgf(iso, LOG_PREFIX_"initializer allocation failure\n");
    return false;
  }
  *ini = (init_t_) {
    .file      = f,
    .refcnt    = 1,
    .self_lock = {
      .file  = f,
      .ex    = true,
      .udata = ini,
      .cb    = init_lock_self_cb_,
    },
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&ini->self_lock))) {
    upd_file_unref(th);
    upd_iso_msgf(iso, LOG_PREFIX_"init lock refusal\n");
    return false;
  }
  return true;
}

static void view_deinit_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  view_t_*   ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->win.ptr)) {
    glfwSetWindowUserPointer(ctx->win.ptr, NULL);
  }

  const bool th_alive = atomic_load(&ctx->thread_alive);
  if (HEDLEY_LIKELY(th_alive)) {
    atomic_store(&ctx->file_alive, false);
  }

  if (HEDLEY_LIKELY(ctx->prog)) {
    upd_file_ref(ctx->th);
    const bool start = upd_iso_start_work(
      iso, view_teardown_gl_, view_teardown_gl_cb_, ctx->th);
    if (HEDLEY_UNLIKELY(!start)) {
      upd_file_unref(ctx->th);
    }
  }

  if (HEDLEY_UNLIKELY(!th_alive)) {
    upd_file_unref(ctx->th);
  }
}

static bool view_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  view_t_*    ctx = f->ctx;

  switch (req->type) {
  case UPD_REQ_PROG_EXEC: {
    if (HEDLEY_UNLIKELY(atomic_load(&ctx->stream))) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }

    upd_file_t* stf = upd_file_new(&(upd_file_t) {
        .iso    = f->iso,
        .driver = &stream_,
      });
    if (HEDLEY_UNLIKELY(stf == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    stream_t_* stctx = stf->ctx;

    stctx->target = f;
    upd_file_ref(f);

    ctx->win.tw     = 0;
    ctx->win.th     = 0;
    ctx->win.aspect = 0;
    atomic_store(&ctx->stream, (uintptr_t) stf);
    view_req_update_(f);

    req->prog.exec = stf;
    req->cb(req);

    upd_file_unref(stf);
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static void view_req_update_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  view_t_*   ctx = f->ctx;

  atomic_store(&ctx->win.dirty, true);
  if (HEDLEY_UNLIKELY(!upd_file_trigger_async(iso, ctx->th->id))) {
    /* ignore errors */
  }
}

static void view_teardown_gl_(void* udata) {
  upd_file_t* f   = udata;
  view_t_*    ctx = f->ctx;

  upd_file_begin_sync(ctx->gl);

  glfwMakeContextCurrent(ctx->win.ptr);
  const char* err;
  if (HEDLEY_UNLIKELY(glfwGetError(&err))) {
    upd_file_end_sync(ctx->gl);

    utf8ncpy(ctx->err, err, sizeof(ctx->err)-1);
    return;
  }

  glDeleteProgram(ctx->prog);
  glDeleteVertexArrays(1, &ctx->vao);
  glDeleteSamplers(1, &ctx->sampler);
  glDeleteFramebuffers(1, &ctx->fbo);
  glDeleteTextures(1, &ctx->tex);

  assert(glGetError() == GL_NO_ERROR);

  glfwMakeContextCurrent(NULL);
  upd_file_end_sync(ctx->gl);
}

static void view_size_cb_(GLFWwindow* win, int w, int h) {
  upd_file_t* f = glfwGetWindowUserPointer(win);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    return;
  }
  view_t_* ctx = f->ctx;

  atomic_store(&ctx->win.w, (uint32_t) w);
  atomic_store(&ctx->win.h, (uint32_t) h);
  view_req_update_(f);
}

static void view_refresh_cb_(GLFWwindow* w) {
  upd_file_t* f = glfwGetWindowUserPointer(w);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    return;
  }
  view_req_update_(f);
}

static void view_teardown_gl_cb_(upd_iso_t* iso, void* udata) {
  upd_file_t* th = udata;
  (void) iso;

  upd_file_unref(th);
}


static bool thread_init_(upd_file_t* f) {
  (void) f;
  return true;
}

static void thread_deinit_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  view_t_*   ctx = f->ctx;

  for (size_t i = 0; i < ctx->done.n; ++i) {
    frame_delete_(ctx->done.p[i]);
  }
  upd_array_clear(&ctx->done);

  if (HEDLEY_UNLIKELY(ctx->win.ptr)) {
    const bool ok = gra_glfw_lock_and_req_with_dup(&(gra_glfw_req_t) {
        .dev   = ctx->glfw,
        .type  = GRA_GLFW_REQ_WIN_DEINIT,
        .win   = ctx->win.ptr,
        .udata = iso,
        .cb    = thread_teardown_glfw_cb_,
      });
    if (HEDLEY_UNLIKELY(!ok)) {
      upd_iso_msgf(iso, LOG_PREFIX_"failed to delete GLFW window\n");
    }
  }
  if (HEDLEY_LIKELY(ctx->gl)) {
    upd_file_unref(ctx->gl);
  }
  if (HEDLEY_LIKELY(ctx->glfw)) {
    upd_file_unref(ctx->glfw);
  }
  upd_free(&ctx);
}

static bool thread_handle_(upd_req_t* req) {
  req->result = UPD_REQ_INVALID;
  return false;
}

static void thread_main_(void* udata) {
  upd_file_t* f   = udata;
  upd_iso_t*  iso = f->iso;
  view_t_*    ctx = f->ctx;

  const upd_file_id_t id = f->id;

  while (atomic_load(&ctx->file_alive)) {
    if (HEDLEY_UNLIKELY(atomic_load(&ctx->win.dirty))) {

      upd_file_begin_sync(ctx->gl);
      glfwMakeContextCurrent(ctx->win.ptr);
      const char* err;
      if (HEDLEY_UNLIKELY(glfwGetError(&err))) {
        upd_file_end_sync(ctx->gl);
        continue;
      }

      const uint32_t ww  = atomic_load(&ctx->win.w);
      const uint32_t wh  = atomic_load(&ctx->win.h);

      upd_file_t* stf = (void*) atomic_load(&ctx->stream);

      if (HEDLEY_LIKELY(stf != NULL)) {
        thread_update_fb_(f, stf);
      }

      const uint32_t tw = ctx->win.tw;
      const uint32_t th = ctx->win.th;

      const double aspect = ctx->win.aspect;

      glBindFramebuffer(GL_READ_FRAMEBUFFER,  ctx->fbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

      glViewport(0, 0, ww, wh);
      glClear(GL_COLOR_BUFFER_BIT);

      if (HEDLEY_UNLIKELY(stf != NULL && tw && th && aspect > 0)) {
        uint32_t sw = ww, sh = ww/aspect;
        if (sw > ww || sh > wh) {
          sw = wh*aspect, sh = wh;
        }

        const uint32_t ox = (ww-sw)/2;
        const uint32_t oy = (wh-sh)/2;
        glBlitFramebuffer(
          0,  0,  tw,    th,
          ox, oy, ox+sw, oy+sh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
      }

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      assert(glGetError() == GL_NO_ERROR);

      glFlush();
      glfwSwapBuffers(ctx->win.ptr);
      glfwMakeContextCurrent(NULL);
      upd_file_end_sync(ctx->gl);

      atomic_store(&ctx->win.dirty, false);
      if (HEDLEY_UNLIKELY(!upd_file_trigger_async(iso, id))) {
        fprintf(stderr, LOG_PREFIX_
          "failed to trigger ASYNC event, "
          "this may cause OpenGL device get stuck X(\n");
      }
    }
  }

  atomic_store(&ctx->thread_alive, false);
  if (HEDLEY_UNLIKELY(!upd_file_trigger_async(iso, id))) {
    fprintf(stderr, LOG_PREFIX_
      "failed to trigger ASYNC event, "
      "this may cause iso machine stuck X(\n");
  }
}

static void thread_update_fb_(upd_file_t* f, upd_file_t* stf) {
  view_t_* ctx = f->ctx;

  stream_t_* st    = stf->ctx;
  frame_t_*  frame = (void*) atomic_exchange(&st->pending, 0);
  if (HEDLEY_LIKELY(frame == NULL)) {
    return;
  }

  const GLuint   t   = frame->tex;
  const uint32_t tw  = frame->w;
  const uint32_t th  = frame->h;
  const uint32_t taw = gra_next_power2(tw);
  const uint32_t tah = gra_next_power2(th);

  ctx->win.tw     = tw;
  ctx->win.th     = th;
  ctx->win.aspect = tw*1./th;

  glBindTexture(GL_TEXTURE_2D, ctx->tex);
  glTexImage2D(GL_TEXTURE_2D,
    0, GL_RGBA, taw, tah, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

  glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
  glFramebufferTexture2D(
    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->tex, 0);
  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  glViewport(0, 0, tw, th);

  if (t) {
    const double zx = tw*1./taw, zy = th*1./tah;

    glUseProgram(ctx->prog);
    glBindVertexArray(ctx->vao);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glBindSampler(0, ctx->sampler);

    glUniform1i(SHADER_UNI_TEX_, 0);
    glUniform2f(SHADER_UNI_SCALE_, 1, 1);
    glUniform2f(SHADER_UNI_SIZE_,  zx, zy);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);

  } else {

    const GLenum fmt  = gra_gl3_dim_to_color_fmt(frame->ch);
    const GLenum type = gra_gl3_tensor_type_to_type(frame->type);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, fmt, type, frame->data);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  assert(glGetError() == GL_NO_ERROR);

  upd_file_begin_sync(f);
  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->done, frame, SIZE_MAX))) {
    fprintf(stderr, "frame context insertion failure, this may cause dead-lock");
  }
  upd_file_end_sync(f);
}

static void thread_watch_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->udata;
  upd_iso_t*  iso = f->iso;
  view_t_*    ctx = f->ctx;

  if (HEDLEY_UNLIKELY(w->event != UPD_FILE_ASYNC)) {
    return;
  }

  upd_file_begin_sync(f);
  {
    for (size_t i = 0; i < ctx->done.n; ++i) {
      frame_delete_(ctx->done.p[i]);
    }
    upd_array_clear(&ctx->done);
  }
  upd_file_end_sync(f);

  if (HEDLEY_UNLIKELY(!atomic_load(&ctx->thread_alive))) {
    if (ctx->err[0]) {
      upd_iso_msgf(iso, "view thread error: %s\n", ctx->err);
    } else {
      upd_iso_msgf(iso, "view thread exited\n");
    }
    upd_file_unwatch(&ctx->watch);
    upd_file_unref(f);
  }
}

static void thread_teardown_glfw_cb_(gra_glfw_req_t* req) {
  upd_iso_t* iso = req->udata;

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}


static bool stream_init_(upd_file_t* f) {
  stream_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (stream_t_) {
    .watch = {
      .file  = f,
      .udata = f,
      .cb    = stream_watch_cb_,
    },
  };
  f->ctx = ctx;

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    return false;
  }

  if (HEDLEY_UNLIKELY(!upd_msgpack_init(&ctx->mpk))) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    return false;
  }
  ctx->mpk.udata = f;
  ctx->mpk.cb    = stream_msgpack_cb_;

  return true;
}

static void stream_deinit_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;
  view_t_*   tar = ctx->target->ctx;

  for (size_t i = 0; i < ctx->waiting.n; ++i) {
    frame_delete_(ctx->waiting.p[i]);
  }
  upd_array_clear(&ctx->waiting);

  atomic_store(&tar->stream, (uintptr_t) NULL);
  view_req_update_(ctx->target);

  upd_file_unref(ctx->target);
  upd_file_unwatch(&ctx->watch);

  upd_free(&ctx);
}

static bool stream_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  stream_t_*  ctx = f->ctx;

  switch (req->type) {
  case UPD_REQ_DSTREAM_READ:
  case UPD_REQ_DSTREAM_WRITE:
    return upd_msgpack_handle(&ctx->mpk, req);

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static void stream_return_(upd_file_t* f, bool ok, const char* msg) {
  stream_t_* ctx = f->ctx;

  msgpack_packer* pk = &ctx->mpk.pk;

  ctx->mpk.broken |=
    msgpack_pack_map(pk, 2)                ||
      upd_msgpack_pack_cstr(pk, "success") ||
        upd_msgpack_pack_bool(pk, ok)      ||
      upd_msgpack_pack_cstr(pk, "msg")     ||
        upd_msgpack_pack_cstr(pk, msg);

  upd_file_trigger(f, UPD_FILE_UPDATE);
  stream_msgpack_cb_(&ctx->mpk);
}

static void stream_queue_(upd_file_t* f, frame_t_* frame) {
  stream_t_* ctx = f->ctx;

  const uint64_t now = upd_iso_now(f->iso);

  if (HEDLEY_UNLIKELY(frame->time <= now)) {
    upd_iso_msgf(f->iso, LOG_PREFIX_"frame dropped X(\n");
    return;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->waiting, frame, SIZE_MAX))) {
    upd_iso_msgf(f->iso, LOG_PREFIX_"frame dropped X(\n");
    return;
  }

  const uint64_t rem = frame->time - now;
  if (HEDLEY_UNLIKELY(!upd_file_trigger_timer(f, rem))) {
    upd_iso_msgf(f->iso, LOG_PREFIX_"frame dropped X(\n");
    return;
  }
}

static void stream_msgpack_cb_(upd_msgpack_t* mpk) {
  upd_file_t* f   = mpk->udata;
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!mpk->busy)) {
    msgpack_unpacked_init(&ctx->upkd);
  }
  if (HEDLEY_UNLIKELY(!upd_msgpack_pop(mpk, &ctx->upkd))) {
    msgpack_unpacked_destroy(&ctx->upkd);
    if (HEDLEY_UNLIKELY(mpk->broken)) {
      upd_file_trigger(f, UPD_FILE_UPDATE);
    }
    if (mpk->busy) {
      mpk->busy = false;
      upd_file_unref(f);
    }
    return;
  }
  if (HEDLEY_UNLIKELY(!mpk->busy)) {
    mpk->busy = true;
    upd_file_ref(f);
  }

  ctx->par = (upd_proto_parse_t) {
    .iso   = f->iso,
    .src   = &ctx->upkd.data,
    .iface = UPD_PROTO_ENCODER,
    .udata = f,
    .cb    = stream_proto_parse_cb_,
  };
  upd_proto_parse(&ctx->par);
}

static void stream_proto_parse_cb_(upd_proto_parse_t* par) {
  upd_file_t* f   = par->udata;
  upd_iso_t*  iso = f->iso;
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(par->err)) {
    stream_return_(f, false, par->err);
    return;
  }

  msgpack_packer* pk = &ctx->mpk.pk;

  const upd_proto_msg_t* msg = &par->msg;
  switch (msg->cmd) {
  case UPD_PROTO_ENCODER_INFO:
    ctx->mpk.broken |=
      msgpack_pack_map(pk, 2)                                                   ||
        upd_msgpack_pack_cstr(pk, "success")                                    ||
          msgpack_pack_true(pk)                                                 ||
        upd_msgpack_pack_cstr(pk, "result")                                     ||
          msgpack_pack_map(pk, 4)                                               ||
            upd_msgpack_pack_cstr(pk, "description")                            ||
              upd_msgpack_pack_cstr(pk, "OpenGL texture presenter")             ||
            upd_msgpack_pack_cstr(pk, "type")                                   ||
              upd_msgpack_pack_cstr(pk, "video")                                ||
            upd_msgpack_pack_cstr(pk, "initParam")                              ||
              msgpack_pack_map(pk, 2)                                           ||
                upd_msgpack_pack_cstr(pk, "utimeNum")                           ||
                msgpack_pack_map(pk, 2)                                         ||
                  upd_msgpack_pack_cstr(pk, "type")                             ||
                  upd_msgpack_pack_cstr(pk, "integer")                          ||
                  upd_msgpack_pack_cstr(pk, "description")                      ||
                  upd_msgpack_pack_cstr(pk, "numerator part of unittime (s)")   ||
                upd_msgpack_pack_cstr(pk, "utimeDen")                           ||
                msgpack_pack_map(pk, 2)                                         ||
                  upd_msgpack_pack_cstr(pk, "type")                             ||
                  upd_msgpack_pack_cstr(pk, "integer")                          ||
                  upd_msgpack_pack_cstr(pk, "description")                      ||
                  upd_msgpack_pack_cstr(pk, "denominator part of unittime (s)") ||
            upd_msgpack_pack_cstr(pk, "frameParam")                             ||
              msgpack_pack_map(pk, 1)                                           ||
                upd_msgpack_pack_cstr(pk, "time")                               ||
                msgpack_pack_map(pk, 2)                                         ||
                  upd_msgpack_pack_cstr(pk, "type")                             ||
                  upd_msgpack_pack_cstr(pk, "integer")                          ||
                  upd_msgpack_pack_cstr(pk, "description")                      ||
                  upd_msgpack_pack_cstr(pk, "actual time when a frame is shown");
    upd_file_trigger(f, UPD_FILE_UPDATE);
    stream_msgpack_cb_(&ctx->mpk);
    return;

  case UPD_PROTO_ENCODER_INIT: {
    if (HEDLEY_UNLIKELY(ctx->init)) {
      stream_return_(f, false, "already initialized");
      return;
    }
    if (HEDLEY_UNLIKELY(msg->param == NULL)) {
      stream_return_(f, false, "param required");
      return;
    }
    const char* invalid =
      upd_msgpack_find_fields(msg->param, (upd_msgpack_field_t[]) {
          { .name = "utimeNum", .ui = &ctx->ut_num, .required = true, },
          { .name = "utimeDen", .ui = &ctx->ut_den, .required = true, },
          { NULL, },
        });
    if (HEDLEY_UNLIKELY(invalid || ctx->ut_num == 0 || ctx->ut_den == 0)) {
      stream_return_(f, false, "invalid param");
      return;
    }
    ctx->tbase   = upd_iso_now(iso);
    ctx->ut_num *= 1000;  /* converts to milliseconds */
    ctx->init    = true;
    stream_return_(f, true, "");
  } return;

  case UPD_PROTO_ENCODER_FRAME: {
    if (HEDLEY_UNLIKELY(!ctx->init)) {
      stream_return_(f, false, "not initialized");
      return;
    }
    if (HEDLEY_UNLIKELY(msg->param == NULL)) {
      stream_return_(f, false, "param required");
      return;
    }

    uintmax_t time = 0;

    const char* invalid =
      upd_msgpack_find_fields(msg->param, (upd_msgpack_field_t[]) {
          { .name = "time", .ui = &time, },
          { NULL, },
        });
    if (HEDLEY_UNLIKELY(invalid)) {
      stream_return_(f, false, "invalid param");
      return;
    }

    const uint64_t now    = upd_iso_now(iso);
    const uint64_t expect = time*ctx->ut_num/ctx->ut_den + ctx->tbase;
    if (HEDLEY_UNLIKELY(expect <= now)) {
      stream_return_(f, false, "frame is too old");
      return;
    }
    if (HEDLEY_UNLIKELY(expect-now >= VIEW_QUEUE_DURATION_)) {
      stream_return_(f, false, "frame is too far");
      return;
    }

    upd_file_t* tensor = msg->encoder_frame.file;

    frame_t_* frame = NULL;
    if (HEDLEY_UNLIKELY(!upd_malloc(&frame, sizeof(*frame)))) {
      stream_return_(f, false, "frame allocation failure");
      return;
    }
    *frame = (frame_t_) {
      .stream = f,
      .tensor = tensor,
      .time   = expect,
      .lock = {
        .file  = tensor,
        .udata = frame,
        .cb    = frame_lock_tensor_cb_,
      },
    };
    upd_file_ref(f);
    if (HEDLEY_UNLIKELY(!upd_file_lock(&frame->lock))) {
      upd_file_unref(f);
      upd_free(&frame);
      stream_return_(f, false, "tensor lock refusal");
      return;
    }
    stream_return_(f, true, "");
  } return;

  case UPD_PROTO_ENCODER_FINALIZE:
    if (HEDLEY_UNLIKELY(!ctx->init)) {
      stream_return_(f, false, "not initialized");
      return;
    }
    ctx->init = false;
    stream_return_(f, true, "");
    return;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }
}

static void stream_watch_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->udata;
  upd_iso_t*  iso = f->iso;
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(w->event != UPD_FILE_TIMER)) {
    return;
  }

  const uint64_t now = upd_iso_now(f->iso);

  frame_t_* frame = NULL;
  while (ctx->waiting.n) {
    frame_t_* next = ctx->waiting.p[0];
    if (HEDLEY_UNLIKELY(now < next->time)) {
      break;
    }
    if (HEDLEY_UNLIKELY(frame)) {
      frame_delete_(frame);
    }
    frame = upd_array_remove(&ctx->waiting, 0);
  }
  if (HEDLEY_UNLIKELY(frame == NULL)) {
    return;
  }

  frame_t_* drop = (void*) atomic_exchange(&ctx->pending, (uintptr_t) frame);
  if (HEDLEY_UNLIKELY(drop)) {
    frame_delete_(drop);
  }

  view_req_update_(ctx->target);

  if (HEDLEY_LIKELY(ctx->waiting.n)) {
    frame_t_* next = ctx->waiting.p[0];
    if (HEDLEY_UNLIKELY(!upd_file_trigger_timer(f, next->time-now))) {
      upd_iso_msgf(iso, LOG_PREFIX_"timer aborted\n");
      return;
    }
  }
}


static void init_setup_gl_(void* udata) {
  init_t_*    ini = udata;
  upd_file_t* f   = ini->file;
  view_t_*    ctx = f->ctx;

  upd_file_begin_sync(ctx->gl);
  glfwMakeContextCurrent(ctx->win.ptr);
  const char* err;
  if (HEDLEY_UNLIKELY(glfwGetError(&err))) {
    upd_file_end_sync(ctx->gl);

    utf8ncpy(ctx->err, err, sizeof(ctx->err)-1);
    return;
  }

  GLuint vsh = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vsh, 1,
    &(const GLchar*) {(GLchar*) gl3_view_vsh}, &(GLsizei) {gl3_view_vsh_len});
  glCompileShader(vsh);

  GLuint fsh = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fsh, 1,
    &(const GLchar*) {(GLchar*) gl3_view_fsh}, &(GLsizei) {gl3_view_fsh_len});
  glCompileShader(fsh);

  ctx->prog = glCreateProgram();
  glAttachShader(ctx->prog, vsh);
  glAttachShader(ctx->prog, fsh);
  glLinkProgram(ctx->prog);

# if !defined(NDEBUG)
    GLint status = 0;

    glGetShaderiv(vsh, GL_COMPILE_STATUS, &status);
    assert(status == GL_TRUE);

    glGetShaderiv(fsh, GL_COMPILE_STATUS, &status);
    assert(status == GL_TRUE);

    glGetProgramiv(ctx->prog, GL_LINK_STATUS, &status);
    assert(status == GL_TRUE);
# endif

  glDeleteShader(vsh);
  glDeleteShader(fsh);

  glGenVertexArrays(1, &ctx->vao);

  GLuint sampler;
  glGenSamplers(1, &sampler);
  glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  ctx->sampler = sampler;

  GLuint tex;
  glGenTextures(1, &tex);
  ctx->tex = tex;

  GLuint fbo;
  glGenFramebuffers(1, &fbo);
  ctx->fbo = fbo;

  assert(glGetError() == GL_NO_ERROR);
  glfwMakeContextCurrent(NULL);
  upd_file_end_sync(ctx->gl);
}

static void init_unref_(init_t_* ini) {
  upd_file_t* f   = ini->file;
  upd_iso_t*  iso = f->iso;
  view_t_*    ctx = f->ctx;

  if (HEDLEY_LIKELY(--ini->refcnt)) {
    return;
  }

  if (HEDLEY_LIKELY(ini->self_locked)) {
    upd_file_unlock(&ini->self_lock);
  }
  if (HEDLEY_LIKELY(ini->gl_locked)) {
    upd_file_unlock(&ini->gl_lock);
  }
  upd_iso_unstack(iso, ini);

  ctx->broken =
    ctx->gl      == NULL ||
    ctx->glfw    == NULL ||
    ctx->win.ptr == NULL ||
    !ctx->prog || !ctx->vao;
  if (HEDLEY_UNLIKELY(ctx->broken)) {
    atomic_store(&ctx->thread_alive, false);
    return;
  }

  ctx->watch = (upd_file_watch_t) {
    .file  = ctx->th,
    .udata = ctx->th,
    .cb    = thread_watch_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    atomic_store(&ctx->thread_alive, false);
    upd_iso_msgf(iso, LOG_PREFIX_"thread watch refusal\n");
    return;
  }
  if (HEDLEY_UNLIKELY(!upd_iso_start_thread(iso, thread_main_, ctx->th))) {
    upd_file_unwatch(&ctx->watch);
    atomic_store(&ctx->thread_alive, false);
    upd_iso_msgf(iso, LOG_PREFIX_"failed to start thread\n");
    return;
  }
  if (HEDLEY_UNLIKELY(!upd_file_trigger_async(iso, ctx->th->id))) {
    /* ignore error */
  }
}

static void init_lock_self_cb_(upd_file_lock_t* k) {
  init_t_*    ini = k->udata;
  upd_file_t* f   = ini->file;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"init lock cancelled");
    goto ABORT;
  }
  ini->self_locked = true;

  const bool ok = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso  = iso,
      .path = (uint8_t*) GRA_GL3_DEV_PATH,
      .len  = sizeof(GRA_GL3_DEV_PATH)-1,
      .udata = ini,
      .cb    = init_pathfind_gl_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GL device pathfind refusal");
    goto ABORT;
  }
  return;

ABORT:
  init_unref_(ini);
}

static void init_pathfind_gl_cb_(upd_pathfind_t* pf) {
  init_t_*    ini = pf->udata;
  upd_file_t* f   = ini->file;
  upd_iso_t*  iso = f->iso;
  view_t_*    ctx = f->ctx;

  upd_file_t* gl = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(gl == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GL device not found: "GRA_GL3_DEV_PATH"\n");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(gl->driver != &gra_gl3_dev)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GL device found, but it's a fake\n");
    goto ABORT;
  }
  ctx->gl = gl;
  upd_file_ref(ctx->gl);

  ini->gl_lock = (upd_file_lock_t) {
    .file  = ctx->gl,
    .ex    = true,
    .udata = ini,
    .cb    = init_lock_gl_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&ini->gl_lock))) {
    upd_iso_msgf(iso, LOG_PREFIX_"GL device lock refusal\n");
    goto ABORT;
  }
  return;

ABORT:
  init_unref_(ini);
}

static void init_lock_gl_cb_(upd_file_lock_t* k) {
  init_t_*    ini = k->udata;
  upd_file_t* f   = ini->file;
  upd_iso_t*  iso = f->iso;
  view_t_*    ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GL device lock cancelled\n");
    goto EXIT;
  }
  ini->gl_locked = true;

  gra_gl3_dev_t* glctx = ctx->gl->ctx;
  if (HEDLEY_UNLIKELY(glctx->glfw == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GLFW device is not available\n");
    goto EXIT;
  }
  ctx->glfw = glctx->glfw;
  upd_file_ref(ctx->glfw);

  ini->glfw_req = (gra_glfw_req_t) {
    .dev  = ctx->glfw,
    .type = GRA_GLFW_REQ_SUB_INIT,
    .sub = {
      .share = glctx->gl,
      .cb    = &win_cb_,
    },
    .udata = ini,
    .cb    = init_win_create_cb_,
  };
  ++ini->refcnt;
  if (HEDLEY_UNLIKELY(!gra_glfw_lock_and_req(&ini->glfw_req))) {
    --ini->refcnt;
    upd_iso_msgf(iso, LOG_PREFIX_"window create request refusal\n");
    goto EXIT;
  }

EXIT:
  init_unref_(ini);
}

static void init_win_create_cb_(gra_glfw_req_t* req) {
  init_t_*    ini = req->udata;
  upd_file_t* f   = ini->file;
  upd_iso_t*  iso = f->iso;
  view_t_*    ctx = f->ctx;

  GLFWwindow* win = req->ok? req->sub.win: NULL;
  upd_file_unlock(&req->lock);

  if (HEDLEY_UNLIKELY(win == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"window create request failure\n");
    goto EXIT;
  }
  ctx->win.ptr = win;
  glfwSetWindowUserPointer(win, f);

  ++ini->refcnt;
  const bool start =
    upd_iso_start_work(iso, init_setup_gl_, init_setup_gl_cb_, ini);
  if (HEDLEY_UNLIKELY(!start)) {
    --ini->refcnt;
    upd_iso_msgf(iso, LOG_PREFIX_"worker thread start failure\n");
    goto EXIT;
  }

EXIT:
  init_unref_(ini);
}

static void init_setup_gl_cb_(upd_iso_t* iso, void* udata) {
  init_t_* ini = udata;

  if (HEDLEY_UNLIKELY(ini->err[0])) {
    upd_iso_msgf(iso, LOG_PREFIX_"OpenGL error: %s\n", ini->err);
    goto EXIT;
  }

EXIT:
  init_unref_(ini);
}


static void frame_delete_(frame_t_* frame) {
  if (HEDLEY_UNLIKELY(frame->fetched)) {
    frame->req = (upd_req_t) {
      .file  = frame->tensor,
      .type  = UPD_REQ_TENSOR_FLUSH,
      .udata = frame,
      .cb    = frame_flush_tensor_cb_,
    };
    if (HEDLEY_LIKELY(upd_req(&frame->req))) {
      return;
    }
  }
  if (HEDLEY_LIKELY(frame->locked)) {
    upd_file_unlock(&frame->lock);
  }
  upd_file_unref(frame->stream);
  upd_free(&frame);
}

static void frame_lock_tensor_cb_(upd_file_lock_t* k) {
  frame_t_*   frame  = k->udata;
  upd_file_t* f      = frame->stream;
  upd_file_t* tensor = frame->tensor;
  upd_iso_t*  iso    = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"tensor lock cancelled\n");
    goto ABORT;
  }
  frame->locked = true;

  if (HEDLEY_UNLIKELY(tensor->driver == &gra_gl3_tex_2d)) {
    gra_gl3_tex_t* texctx = tensor->ctx;
    frame->tex = texctx->id;
    frame->w   = texctx->w;
    frame->h   = texctx->h;
    stream_queue_(f, frame);
    return;
  }

  frame->req = (upd_req_t) {
    .file  = tensor,
    .type  = UPD_REQ_TENSOR_FETCH,
    .udata = frame,
    .cb    = frame_fetch_tensor_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&frame->req))) {
    upd_iso_msgf(iso, LOG_PREFIX_"tensor fetch refusal\n");
    goto ABORT;
  }
  return;

ABORT:
  frame_delete_(frame);
}

static void frame_fetch_tensor_cb_(upd_req_t* req) {
  frame_t_*   frame = req->udata;
  upd_file_t* f     = frame->stream;
  upd_iso_t*  iso   = f->iso;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    upd_iso_msgf(iso, LOG_PREFIX_"tensor fetch failure\n");
    goto ABORT;
  }
  frame->fetched = true;

  const upd_req_tensor_data_t* data = &req->tensor.data;
  if (HEDLEY_UNLIKELY(data->meta.rank != 3)) {
    upd_iso_msgf(iso, LOG_PREFIX_"tensor rank must be 3\n");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(data->meta.type == UPD_TENSOR_F64)) {
    upd_iso_msgf(iso, LOG_PREFIX_"f64 tensor is not supported X(\n");
    goto ABORT;
  }

  frame->ch   = data->meta.reso[0];
  frame->w    = data->meta.reso[1];
  frame->h    = data->meta.reso[2];
  frame->type = data->meta.type;
  frame->data = data->ptr;
  stream_queue_(f, frame);
  return;

ABORT:
  frame_delete_(frame);
}

static void frame_flush_tensor_cb_(upd_req_t* req) {
  frame_t_* frame = req->udata;

  frame->fetched = false;
  frame_delete_(frame);
}
