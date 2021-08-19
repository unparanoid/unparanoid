#include "common.h"

#define LOG_PREFIX_ "upd.graphics.gl3.glsl: "


static
bool
frag_init_(
  upd_file_t* f);

static
bool
vert_init_(
  upd_file_t* f);

static
bool
glsl_init_(
  upd_file_t* f,
  GLenum      target);

static
void
glsl_deinit_(
  upd_file_t* f);

static
bool
glsl_handle_(
  upd_req_t* req);

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
glsl_logf_(
  upd_file_t* f,
  const char* fmt,
  ...);

static
void
glsl_finalize_fetch_(
  upd_file_t* f,
  bool        ok);

const upd_driver_t gra_gl3_glsl_fragment = {
  .name   = (uint8_t*) "upd.graphics.gl3.glsl.fragment",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_PROG,
    0,
  },
  .init   = frag_init_,
  .deinit = glsl_deinit_,
  .handle = glsl_handle_,
};

const upd_driver_t gra_gl3_glsl_vertex = {
  .name   = (uint8_t*) "upd.graphics.gl3.glsl.vertex",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_PROG,
    0,
  },
  .init   = vert_init_,
  .deinit = glsl_deinit_,
  .handle = glsl_handle_,
};


static
void
glsl_lock_for_init_cb_(
  upd_file_lock_t* k);

static
void
glsl_pathfind_dev_cb_(
  upd_pathfind_t* pf);

static
void
glsl_watch_bin_cb_(
  upd_file_watch_t* w);

static
void
glsl_shader_renew_cb_(
  gra_gl3_req_t* req);

static
void
glsl_lock_bin_for_read_cb_(
  upd_file_lock_t* k);

static
void
glsl_read_bin_cb_(
  upd_req_t* req);

static
void
glsl_compile_cb_(
  gra_gl3_req_t* req);

static
void
glsl_del_cb_(
  gra_gl3_req_t* req);


bool gra_gl3_glsl_fetch(gra_gl3_fetch_t* fe) {
  upd_file_t*     f   = fe->file;
  gra_gl3_glsl_t* ctx = f->ctx;

  fe->ok = false;
  if (HEDLEY_UNLIKELY(!ctx->gl)) {
    return false;
  }
  if (HEDLEY_LIKELY(ctx->clean)) {
    if (HEDLEY_UNLIKELY(ctx->broken)) {
      return false;
    }

    fe->glsl = ctx->id;

    fe->ok = true;
    fe->cb(fe);
    return true;
  }

  const bool busy = ctx->pending.n;
  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->pending, fe, SIZE_MAX))) {
    return false;
  }
  if (HEDLEY_UNLIKELY(busy)) {
    return true;
  }

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev  = ctx->gl,
      .type = GRA_GL3_REQ_SHADER_RENEW,
      .shader = {
        .target = ctx->target,
      },
      .udata = f,
      .cb    = glsl_shader_renew_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    glsl_logf_(f, "GL3_REQ_SHADER_RENEW failure");
    upd_array_clear(&ctx->pending);
    return false;
  }
  return true;
}


static bool vert_init_(upd_file_t* f) {
  return glsl_init_(f, GL_VERTEX_SHADER);
}

static bool frag_init_(upd_file_t* f) {
  return glsl_init_(f, GL_FRAGMENT_SHADER);
}

static bool glsl_init_(upd_file_t* f, GLenum target) {
  if (HEDLEY_UNLIKELY(f->npath == NULL)) {
    glsl_logf_(f, "empty npath");
    return false;
  }
  if (HEDLEY_UNLIKELY(f->backend == NULL)) {
    glsl_logf_(f, "no backend file specified");
    return false;
  }

  gra_gl3_glsl_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    glsl_logf_(f, "context allocation failure");
    return false;
  }
  *ctx = (gra_gl3_glsl_t) {
    .target = target,
    .watch  = {
      .file  = f->backend,
      .udata = f,
      .cb    = glsl_watch_bin_cb_,
    },
    .broken = true,
  };

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    glsl_logf_(f, "backend watch failure");
    upd_free(&ctx);
    return false;
  }
  f->ctx = ctx;

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .ex    = true,
      .udata = f,
      .cb    = glsl_lock_for_init_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    glsl_logf_(f, "init lock refusal");
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    return false;
  }
  return true;
}

static void glsl_deinit_(upd_file_t* f) {
  gra_gl3_glsl_t* ctx = f->ctx;
  upd_iso_t*      iso = f->iso;

  if (HEDLEY_LIKELY(ctx->gl && ctx->id)) {
    const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
        .dev  = ctx->gl,
        .type = GRA_GL3_REQ_SHADER_DELETE,
        .shader = {
          .id     = ctx->id,
          .target = ctx->target,
        },
        .udata = iso,
        .cb    = glsl_del_cb_,
      });
    if (HEDLEY_UNLIKELY(!ok)) {
      glsl_logf_(f, "GL3_REQ_SHADER_DELETE failure");
    }
  }
  if (HEDLEY_LIKELY(ctx->gl)) {
    upd_file_unref(ctx->gl);
  }
  upd_file_unwatch(&ctx->watch);
  upd_free(&ctx);
}

static bool glsl_handle_(upd_req_t* req) {
  upd_file_t* f = req->file;

  req->file = f->backend;
  return upd_req(req);
}
static void glsl_logf_(upd_file_t* f, const char* fmt, ...) {
  uint8_t temp[256];

  va_list args;
  va_start(args, fmt);
  vsnprintf((char*) temp, sizeof(temp), fmt, args);
  va_end(args);

  upd_iso_msgf(f->iso, "upd.graphics.gl3.glsl: %s (%s)\n", temp, f->npath);
}

static void glsl_finalize_fetch_(upd_file_t* f, bool ok) {
  gra_gl3_glsl_t* ctx = f->ctx;

  upd_array_t pens = ctx->pending;
  ctx->pending = (upd_array_t) {0};

  for (size_t i = 0; i < pens.n; ++i) {
    gra_gl3_fetch_t* fe = pens.p[i];
    fe->ok   = ok;
    fe->glsl = ctx->id;
    fe->cb(fe);
  }
  upd_array_clear(&pens);
}


static void glsl_lock_for_init_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    glsl_logf_(f, "init lock cancelled");
    goto ABORT;
  }

  const uint8_t* path    = (void*) GRA_GL3_DEV_PATH;
  size_t         pathlen = sizeof(GRA_GL3_DEV_PATH)-1;
  if (HEDLEY_UNLIKELY(f->param)) {
    path    = f->param;
    pathlen = f->paramlen;
  }

  const bool ok = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso   = iso,
      .path  = path,
      .len   = pathlen,
      .udata = k,
      .cb    = glsl_pathfind_dev_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    glsl_logf_(f, "device pathfind refusal");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void glsl_pathfind_dev_cb_(upd_pathfind_t* pf) {
  upd_file_lock_t* k   = pf->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_glsl_t*  ctx = f->ctx;

  upd_file_t* gl = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(gl == NULL)) {
    glsl_logf_(f, "no OpenGL device file found");
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(gl->driver != &gra_gl3_dev)) {
    glsl_logf_(f, "found OpenGL device but it's fake");
    goto EXIT;
  }

  upd_file_ref(gl);
  ctx->gl = gl;

EXIT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void glsl_watch_bin_cb_(upd_file_watch_t* w) {
  upd_file_t*     f   = w->udata;
  gra_gl3_glsl_t* ctx = f->ctx;

  switch (w->event) {
  case UPD_FILE_UPDATE:
    ctx->clean = false;
    upd_file_trigger(f, UPD_FILE_UPDATE);
    break;
  }
}

static void glsl_shader_renew_cb_(gra_gl3_req_t* req) {
  upd_file_t*      f   = req->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_glsl_t*  ctx = f->ctx;

  ctx->id = req->ok? req->shader.id: 0;

  ctx->broken = !ctx->id;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    glsl_logf_(f, "shader renewal failure");
    goto ABORT;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f->backend,
      .udata = req,
      .cb    = glsl_lock_bin_for_read_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    glsl_logf_(f, "backend lock refusal");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);

  glsl_finalize_fetch_(f, false);
}

static void glsl_lock_bin_for_read_cb_(upd_file_lock_t* k) {
  gra_gl3_req_t* req = k->udata;
  upd_file_t*    f   = req->udata;
  upd_iso_t*     iso = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    glsl_logf_(f, "backend lock cancelled");
    goto ABORT;
  }

  const bool read = upd_req_with_dup(&(upd_req_t) {
      .file = f->backend,
      .type = UPD_REQ_STREAM_READ,
      .stream = { .io = {
        .size = SIZE_MAX,
      }, },
      .udata = k,
      .cb    = glsl_read_bin_cb_,
    });
  if (HEDLEY_UNLIKELY(!read)) {
    glsl_logf_(f, "backend read refusal");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);

  glsl_finalize_fetch_(f, false);
}

static void glsl_read_bin_cb_(upd_req_t* req) {
  upd_file_lock_t* k    = req->udata;
  gra_gl3_req_t*   greq = k->udata;
  upd_file_t*      f    = greq->udata;
  upd_iso_t*       iso  = f->iso;
  gra_gl3_glsl_t*  ctx  = f->ctx;

  const bool     ok  = req->result == UPD_REQ_OK;
  const uint8_t* src = req->stream.io.buf;
  const size_t   len = req->stream.io.size;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!ok)) {
    upd_file_unlock(k);
    upd_iso_unstack(iso, k);

    glsl_logf_(f, "backend read failure");
    goto ABORT;
  }

  /* GL device driver has been locked since SHADER_RENEW req */
  const char* err = NULL;
  if (HEDLEY_UNLIKELY(!gra_gl3_dev_make_ctx_current(ctx->gl, &err))) {
    upd_file_unlock(k);
    upd_iso_unstack(iso, k);

    glsl_logf_(f, "OpenGL ctx activation error: %s", err);
    goto ABORT;
  }

  const GLchar* gsrc = (void*) src;
  const GLint   glen = len;
  glShaderSource(ctx->id, 1, &gsrc, &glen);
  assert(glGetError() == GL_NO_ERROR);

  upd_file_unlock(k);
  upd_iso_unstack(iso, k);

  /* reuse greq with preserving greq->lock */
  greq->type = GRA_GL3_REQ_SHADER_COMPILE;
  greq->cb   = glsl_compile_cb_;

  greq->shader.id     = ctx->id;
  greq->shader.target = ctx->target;

  if (HEDLEY_UNLIKELY(!gra_gl3_req(greq))) {
    glsl_logf_(f, "GL3_REQ_SHADER_COMPILE failure");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(&greq->lock);
  upd_iso_unstack(iso, greq);

  glsl_finalize_fetch_(f, false);
}

static void glsl_compile_cb_(gra_gl3_req_t* req) {
  upd_file_t*      f   = req->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_glsl_t*  ctx = f->ctx;

  ctx->broken = !req->ok;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    glsl_logf_(f, "compile failed");
  }
  glsl_finalize_fetch_(f, !ctx->broken);
}

static void glsl_del_cb_(gra_gl3_req_t* req) {
  upd_iso_t* iso = req->udata;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}
