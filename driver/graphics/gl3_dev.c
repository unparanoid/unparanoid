#include "common.h"

#define LOG_PREFIX_ "upd.graphics.gl3.dev: "


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

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
dev_errf_(
  upd_file_t* f,
  const char* fmt,
  ...);

const upd_driver_t gra_gl3_dev = {
  .name   = (uint8_t*) "upd.graphics.gl3.dev",
  .cats   = (upd_req_cat_t[]) {0},
  .init   = dev_init_,
  .deinit = dev_deinit_,
  .handle = dev_handle_,
};


static
void
dev_work_main_(
  void* udata);

static
void
dev_work_handle_buf_alloc_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_buf_map_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_buf_unmap_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_buf_map_pbo_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_buf_unmap_pbo_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_tex_alloc_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_shader_compile_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_prog_link_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_pl_link_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_pl_unlink_(
  upd_file_t*    f,
  gra_gl3_req_t* req);

static
void
dev_work_handle_pl_exec_(
  upd_file_t*    f,
  gra_gl3_req_t* req);


static
void
dev_init_lock_cb_(
  upd_file_lock_t* k);

static
void
dev_init_pathfind_cb_(
  upd_pathfind_t* pf);

static
void
dev_init_gl3_cb_(
  gra_glfw_req_t* req);

static
void
dev_deinit_gl3_cb_(
  gra_glfw_req_t* req);


static
void
dev_work_cb_(
  upd_iso_t* iso,
  void*      udata);


bool gra_gl3_req(gra_gl3_req_t* req) {
  upd_file_t* f   = req->dev;
  upd_iso_t*  iso = f->iso;
  return upd_iso_start_work(iso, dev_work_main_, dev_work_cb_, req);
}


static bool dev_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  gra_gl3_dev_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    upd_iso_msgf(iso, LOG_PREFIX_"context allocation failure\n");
    return false;
  }
  *ctx = (gra_gl3_dev_t) {0};
  f->ctx = ctx;

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .ex    = true,
      .udata = f,
      .cb    = dev_init_lock_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_iso_msgf(iso, LOG_PREFIX_"init lock failure\n");
    return false;
  }
  return true;
}

static void dev_deinit_(upd_file_t* f) {
  upd_iso_t*     iso = f->iso;
  gra_gl3_dev_t* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->glfw && ctx->gl)) {
    const bool req = gra_glfw_lock_and_req_with_dup(&(gra_glfw_req_t) {
        .dev   = ctx->glfw,
        .type  = GRA_GLFW_REQ_WIN_DEINIT,
        .win   = ctx->gl,
        .udata = iso,
        .cb    = dev_deinit_gl3_cb_,
      });
    (void) req;
  }
  if (HEDLEY_LIKELY(ctx->glfw)) {
    upd_file_unref(ctx->glfw);
  }
  upd_free(&ctx);
}

static bool dev_handle_(upd_req_t* req) {
  (void) req;
  return false;
}

static void dev_errf_(upd_file_t* f, const char* fmt, ...) {
  gra_glfw_dev_t* ctx = f->ctx;

  va_list args;
  va_start(args, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, args);
  va_end(args);
}


static void dev_work_main_(void* udata) {
  gra_gl3_req_t* req = udata;
  upd_file_t*    f   = req->dev;

  req->ok = false;

  const char* err = NULL;
  if (HEDLEY_UNLIKELY(!gra_gl3_dev_make_ctx_current(f, &err))) {
    dev_errf_(f, "GL ctx activation error: %s", err);
    return;
  }

  assert(glGetError() == GL_NO_ERROR);
  switch (req->type) {
  case GRA_GL3_REQ_BUF_NEW:
    glGenBuffers(req->buf_multi.n, req->buf_multi.p);
    req->ok = true;
    break;
  case GRA_GL3_REQ_BUF_DELETE:
    glDeleteBuffers(req->buf_multi.n, req->buf_multi.p);
    req->ok = true;
    break;

  case GRA_GL3_REQ_BUF_ALLOC:
    dev_work_handle_buf_alloc_(f, req);
    break;
  case GRA_GL3_REQ_BUF_MAP:
    dev_work_handle_buf_map_(f, req);
    break;
  case GRA_GL3_REQ_BUF_UNMAP:
    dev_work_handle_buf_unmap_(f, req);
    break;

  case GRA_GL3_REQ_BUF_MAP_PBO:
    dev_work_handle_buf_map_pbo_(f, req);
    break;
  case GRA_GL3_REQ_BUF_UNMAP_PBO:
    dev_work_handle_buf_unmap_pbo_(f, req);
    break;

  case GRA_GL3_REQ_TEX_NEW:
    glGenTextures(1, &req->tex.id);
    req->ok = true;
    break;
  case GRA_GL3_REQ_TEX_DELETE:
    glDeleteTextures(1, &req->tex.id);
    req->ok = true;
    break;
  case GRA_GL3_REQ_TEX_ALLOC:
    dev_work_handle_tex_alloc_(f, req);
    break;

  case GRA_GL3_REQ_SHADER_RENEW:
    req->shader.id = glCreateShader(req->shader.target);
    req->ok = true;
    break;
  case GRA_GL3_REQ_SHADER_DELETE:
    glDeleteShader(req->shader.id);
    req->ok = true;
    break;
  case GRA_GL3_REQ_SHADER_COMPILE:
    dev_work_handle_shader_compile_(f, req);
    break;

  case GRA_GL3_REQ_PROG_CREATE:
    req->prog.id = glCreateProgram();
    req->ok = true;
    break;
  case GRA_GL3_REQ_PROG_DELETE:
    glDeleteProgram(req->prog.id);
    req->ok = true;
    break;
  case GRA_GL3_REQ_PROG_ATTACH:
    glAttachShader(req->prog.id, req->prog.shader);
    req->ok = true;
    break;
  case GRA_GL3_REQ_PROG_LINK:
    dev_work_handle_prog_link_(f, req);
    break;

  case GRA_GL3_REQ_PL_LINK:
    dev_work_handle_pl_link_(f, req);
    break;
  case GRA_GL3_REQ_PL_UNLINK:
    dev_work_handle_pl_unlink_(f, req);
    break;
  case GRA_GL3_REQ_PL_EXEC:
    dev_work_handle_pl_exec_(f, req);
    break;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }

  glFlush();
  assert(glGetError() == GL_NO_ERROR);
}

static void dev_work_handle_buf_alloc_(upd_file_t* f, gra_gl3_req_t* req) {
  (void) f;

  const GLuint id     = req->buf_alloc.id;
  const GLenum target = req->buf_alloc.target;
  glBindBuffer(target, id);

  const GLenum usage = req->buf_alloc.usage;
  const void*  data  = req->buf_alloc.data;
  const size_t size  = req->buf_alloc.size;
  glBufferData(target, size, data, usage);
}

static void dev_work_handle_buf_map_(upd_file_t* f, gra_gl3_req_t* req) {
  const GLuint id     = req->buf_map.id;
  const GLenum target = req->buf_map.target;
  glBindBuffer(target, id);

  const GLenum mode = req->buf_map.mode;
  void* data = glMapBuffer(target, mode);
  if (HEDLEY_UNLIKELY(data == NULL)) {
    dev_errf_(f, "buffer map failure");
    return;
  }
  req->buf_map.data = data;
  req->ok = true;
}

static void dev_work_handle_buf_unmap_(upd_file_t* f, gra_gl3_req_t* req) {
  const GLuint id     = req->buf_map.id;
  const GLenum target = req->buf_map.target;
  glBindBuffer(target, id);
  if (HEDLEY_UNLIKELY(GL_FALSE == glUnmapBuffer(target))) {
    dev_errf_(f, "buffer unmap failure");
    return;
  }
  req->ok = true;
}

static void dev_work_handle_buf_map_pbo_(upd_file_t* f, gra_gl3_req_t* req) {
  const GLuint id = req->buf_map_pbo.id;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, id);

  glBufferData(
    GL_PIXEL_PACK_BUFFER, req->buf_map_pbo.size, NULL, GL_DYNAMIC_DRAW);

  const GLuint tex    = req->buf_map_pbo.tex;
  const GLenum target = req->buf_map_pbo.tex_target;
  glBindTexture(target, tex);

  const GLenum fmt  = req->buf_map_pbo.fmt;
  const GLenum type = req->buf_map_pbo.type;
  glGetTexImage(target, 0, fmt, type, 0);

  void* data = glMapBuffer(GL_PIXEL_PACK_BUFFER, req->buf_map_pbo.mode);
  if (HEDLEY_UNLIKELY(data == NULL)) {
    dev_errf_(f, "PBO map failure");
    return;
  }
  req->buf_map_pbo.data = data;
  req->ok = true;
}

static void dev_work_handle_buf_unmap_pbo_(upd_file_t* f, gra_gl3_req_t* req) {
  const GLuint id = req->buf_map_pbo.id;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, id);

  if (HEDLEY_UNLIKELY(GL_FALSE == glUnmapBuffer(GL_PIXEL_PACK_BUFFER))) {
    dev_errf_(f, "PBO unmap corruption");
    return;
  }
  if (req->buf_map_pbo.mode == GL_READ_ONLY) {
    req->ok = true;
    return;
  }

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, id);

  const uint32_t w = req->buf_map_pbo.w;
  const uint32_t h = req->buf_map_pbo.h;
  const uint32_t d = req->buf_map_pbo.d;

  const GLenum target = req->buf_map_pbo.tex_target;
  const GLenum fmt    = req->buf_map_pbo.fmt;
  const GLenum type   = req->buf_map_pbo.type;
  switch (target) {
  case GL_TEXTURE_1D:
    glTexSubImage1D(target, 0, 0, w, fmt, type, 0);
    break;
  case GL_TEXTURE_2D:
    glTexSubImage2D(target, 0, 0, 0, w, h, fmt, type, 0);
    break;
  case GL_TEXTURE_3D:
    glTexSubImage3D(target, 0, 0, 0, 0, w, h, d, fmt, type, 0);
    break;
  }
  req->ok = true;
}

static void dev_work_handle_tex_alloc_(upd_file_t* f, gra_gl3_req_t* req) {
  (void) f;

  const GLenum target = req->tex.target;
  const GLuint id     = req->tex.id;
  glBindTexture(target, id);

  const uint32_t w = req->tex.w;
  const uint32_t h = req->tex.h;
  const uint32_t d = req->tex.d;

  const GLenum fmt    = req->tex.fmt;
  const GLenum type   = req->tex.type;

  void* data = req->tex.data;

  switch (target) {
  case GL_TEXTURE_1D:
    glTexImage1D(target, 0, GL_RGBA, w, 0, fmt, type, data);
    break;

  case GL_TEXTURE_2D:
    glTexImage2D(target, 0, GL_RGBA, w, h, 0, fmt, type, data);
    break;

  case GL_TEXTURE_3D:
    glTexImage3D(target, 0, GL_RGBA, w, h, d, 0, fmt, type, data);
    break;
  }
  req->ok = true;
}

static void dev_work_handle_shader_compile_(upd_file_t* f, gra_gl3_req_t* req) {
  gra_gl3_dev_t* ctx = f->ctx;

  const GLuint id = req->shader.id;
  glCompileShader(id);

  GLint status = 0;
  glGetShaderiv(id, GL_COMPILE_STATUS, &status);
  if (HEDLEY_UNLIKELY(status == GL_FALSE)) {
    glGetShaderInfoLog(id, sizeof(ctx->error), NULL, (GLchar*) ctx->error);
    return;
  }
  req->ok = true;
}

static void dev_work_handle_prog_link_(upd_file_t* f, gra_gl3_req_t* req) {
  gra_gl3_dev_t* ctx = f->ctx;

  const GLuint id = req->prog.id;
  glLinkProgram(id);

  GLint status = 0;
  glGetProgramiv(id, GL_LINK_STATUS, &status);
  if (HEDLEY_UNLIKELY(status == GL_FALSE)) {
    glGetProgramInfoLog(id, sizeof(ctx->error), NULL, (GLchar*) ctx->error);
    return;
  }
  req->ok = true;
}

static void dev_work_handle_pl_link_(upd_file_t* f, gra_gl3_req_t* req) {
  gra_gl3_dev_t*      ctx  = f->ctx;
  const gra_gl3_pl_t* pl   = req->pl.ptr;
  uint8_t*            vbuf = req->pl.varbuf;

  GLuint obj[32] = {0};
  size_t obj_rem = 0;

  /* TODO create rb */

  for (size_t i = 0; i < pl->fbcnt; ++i) {
    const gra_gl3_pl_fb_t* fb = &pl->fb[i];
    if (HEDLEY_UNLIKELY(obj_rem == 0)) {
      const size_t require = pl->fbcnt - i;

      obj_rem = sizeof(obj)/sizeof(obj[0]);
      if (obj_rem > require) obj_rem = require;

      glGenFramebuffers(obj_rem, obj);
    }

    const GLuint id = obj[--obj_rem];
    *(GLuint*) (vbuf + fb->var->offset) = id;
    if (HEDLEY_UNLIKELY(id == 0)) {
      dev_errf_(f, "object allocation failure");
      continue;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, id);
    for (size_t j = 0; j < fb->attachcnt; ++j) {
      const gra_gl3_pl_fb_attach_t* a = &fb->attach[j];
      if (HEDLEY_UNLIKELY(a->var == NULL)) {
        break;
      }

      const GLuint refid = *(GLuint*) (vbuf + a->var->offset);
      if (HEDLEY_UNLIKELY(refid == 0)) {
        dev_errf_(f, "fb refers null");
        continue;
      }

      switch (a->var->type) {
      case GRA_GL3_PL_VAR_RB:
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, a->type, GL_RENDERBUFFER, refid);
        break;
      case GRA_GL3_PL_VAR_TEX2:
        glFramebufferTexture2D(GL_FRAMEBUFFER, a->type, GL_TEXTURE_2D, refid, 0);
        break;
      default:
        assert(false);
        HEDLEY_UNREACHABLE();
      }
      assert(glGetError() == GL_NO_ERROR);
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (HEDLEY_UNLIKELY(status != GL_FRAMEBUFFER_COMPLETE)) {
      dev_errf_(f, "fb status invalid");
      continue;
    }
  }

  for (size_t i = 0; i < pl->vacnt; ++i) {
    const gra_gl3_pl_va_t* va = &pl->va[i];

    if (HEDLEY_UNLIKELY(obj_rem == 0)) {
      const size_t require = pl->vacnt - i;

      obj_rem = sizeof(obj)/sizeof(obj[0]);
      if (obj_rem > require) obj_rem = require;

      glGenVertexArrays(obj_rem, obj);
    }

    const GLuint id = obj[--obj_rem];
    *(GLuint*) (vbuf + va->var->offset) = id;
    if (HEDLEY_UNLIKELY(id == 0)) {
      dev_errf_(f, "vertex array allocation failure");
      continue;
    }

    glBindVertexArray(id);
    for (size_t j = 0; j < GRA_GL3_PL_VA_ATTACH_MAX; ++j) {
      const gra_gl3_pl_va_attach_t* a = &va->attach[j];
      if (HEDLEY_LIKELY(a->buf == NULL)) {
        continue;
      }

      const GLuint buf = *(GLuint*) (vbuf + a->buf->offset);
      if (HEDLEY_UNLIKELY(buf == 0)) {
        continue;
      }

      intmax_t stride = 0;
      const bool stridevalid =
        gra_gl3_pl_get_value(&a->stride, vbuf, &stride, NULL);
      if (HEDLEY_UNLIKELY(!stridevalid || stride < 0)) {
        dev_errf_(f, "va stride invalid");
        continue;
      }

      intmax_t offset = 0;
      const bool offsetvalid =
        gra_gl3_pl_get_value(&a->offset, vbuf, &offset, NULL);
      if (HEDLEY_UNLIKELY(!offsetvalid || offset < 0)) {
        dev_errf_(f, "va offset invalid");
        continue;
      }

      intmax_t divisor = 0;
      const bool divisorvalid =
        gra_gl3_pl_get_value(&a->divisor, vbuf, &divisor, NULL);
      if (HEDLEY_UNLIKELY(!divisorvalid || divisor < 0)) {
        dev_errf_(f, "va divisor invalid");
        continue;
      }

      glBindBuffer(GL_ARRAY_BUFFER, buf);
      glEnableVertexAttribArray(j);
      glVertexAttribPointer(
        j, a->dim, a->type, GL_FALSE, stride, (GLvoid*) offset);
      glVertexAttribDivisor(j, divisor);

      assert(glGetError() == GL_NO_ERROR);
    }
  }

  req->ok = !ctx->error[0];
  if (HEDLEY_UNLIKELY(!req->ok)) {
    dev_work_handle_pl_unlink_(f, req);
    req->ok = false;
  }
}

static void dev_work_handle_pl_unlink_(upd_file_t* f, gra_gl3_req_t* req) {
  const gra_gl3_pl_t* pl   = req->pl.ptr;
  uint8_t*            vbuf = req->pl.varbuf;
  (void) f;

  GLuint obj[32] = {0};
  size_t obj_cnt = 0;

  for (size_t i = 0; i < pl->fbcnt; ++i) {
    const gra_gl3_pl_fb_t* fb = &pl->fb[i];
    if (HEDLEY_UNLIKELY(fb->var == NULL)) {
      continue;
    }

    if (HEDLEY_UNLIKELY(obj_cnt >= sizeof(obj)/sizeof(obj[0]))) {
      glDeleteFramebuffers(obj_cnt, obj);
      obj_cnt = 0;
    }
    const GLuint id = *(GLuint*) (vbuf + fb->var->offset);
    if (HEDLEY_LIKELY(id)) {
      obj[obj_cnt++] = id;
    }
  }
  glDeleteFramebuffers(obj_cnt, obj);
  obj_cnt = 0;

  for (size_t i = 0; i < pl->vacnt; ++i) {
    const gra_gl3_pl_va_t* va = &pl->va[i];
    if (HEDLEY_UNLIKELY(va->var == NULL)) {
      continue;
    }

    if (HEDLEY_UNLIKELY(obj_cnt >= sizeof(obj)/sizeof(obj[0]))) {
      glDeleteVertexArrays(obj_cnt, obj);
      obj_cnt = 0;
    }
    const GLuint id = *(GLuint*) (vbuf + va->var->offset);
    if (HEDLEY_LIKELY(id)) {
      obj[obj_cnt++] = id;
    }
  }
  glDeleteVertexArrays(obj_cnt, obj);
  obj_cnt = 0;

  req->ok = true;
}

static void dev_work_handle_pl_exec_(upd_file_t* f, gra_gl3_req_t* req) {
  const gra_gl3_pl_t* pl   = req->pl.ptr;
  const uint8_t*      vbuf = req->pl.varbuf;

  glEnable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);

  for (size_t i = 0; i < pl->stepcnt; ++i) {
    const gra_gl3_step_t* step = &pl->step[i];
    switch (step->type) {
    case GRA_GL3_STEP_NONE:
      break;

    case GRA_GL3_STEP_CLEAR: {
      const gra_gl3_step_clear_t* clear = &step->clear;
      const gra_gl3_pl_fb_t*      fb    = clear->fb;

      const GLuint fbid = *(GLuint*) (vbuf + fb->var->offset);
      glBindFramebuffer(GL_FRAMEBUFFER, fbid);

      glClear(step->clear.bits);
    } break;

    case GRA_GL3_STEP_DRAW: {
      const gra_gl3_step_draw_t* draw = &step->draw;
      const gra_gl3_pl_fb_t*     fb   = draw->fb;
      const gra_gl3_pl_va_t*     va   = draw->va;

      glUseProgram(draw->prog);

      const GLuint fbid = *(GLuint*) (vbuf + fb->var->offset);
      const GLuint vaid = *(GLuint*) (vbuf + va->var->offset);

      glBindFramebuffer(GL_FRAMEBUFFER, fbid);
      glBindVertexArray(vaid);

      for (size_t j = 0; j < GRA_GL3_STEP_DRAW_UNI_MAX; ++j) {
        const gra_gl3_pl_value_t* val = &draw->uni[j];
        switch (val->type) {
        case GRA_GL3_PL_VALUE_NONE:
          break;

        case GRA_GL3_PL_VALUE_REF:
          assert(false); /* TODO not implemented */
          break;

        case GRA_GL3_PL_VALUE_INTEGER:
          glUniform1i(j, val->i);
          break;

        case GRA_GL3_PL_VALUE_SCALAR:
          glUniform1f(j, val->f);
          break;

        default:
          assert(false);
          HEDLEY_UNREACHABLE();
        }
      }

      intmax_t count = 0;
      const bool countvalid =
        gra_gl3_pl_get_value(&draw->count, vbuf, &count, NULL);
      if (HEDLEY_UNLIKELY(!countvalid || count < 0)) {
        dev_errf_(f, "invalid draw count");
        return;
      }

      intmax_t inst = 0;
      const bool instvalid =
        gra_gl3_pl_get_value(&draw->instance, vbuf, &inst, NULL);
      if (HEDLEY_UNLIKELY(!instvalid || inst < 0)) {
        dev_errf_(f, "invalid draw instance");
        return;
      }

      const GLboolean r = draw->blend.mask.r? GL_TRUE: GL_FALSE;
      const GLboolean g = draw->blend.mask.g? GL_TRUE: GL_FALSE;
      const GLboolean b = draw->blend.mask.b? GL_TRUE: GL_FALSE;
      const GLboolean a = draw->blend.mask.a? GL_TRUE: GL_FALSE;
      glColorMask(r, g, b, a);
      glBlendEquation(draw->blend.eq);
      glBlendFunc(draw->blend.src, draw->blend.dst);

      glDepthMask(draw->depth.mask? GL_TRUE: GL_FALSE);
      glDepthFunc(draw->depth.func);

      intmax_t w, h;
      const bool vpvalid =
        gra_gl3_pl_get_value(&draw->viewport[0], vbuf, &w, NULL) &&
        gra_gl3_pl_get_value(&draw->viewport[1], vbuf, &h, NULL) &&
        w > 0 && h > 0;
      if (HEDLEY_UNLIKELY(!vpvalid)) {
        dev_errf_(f, "invalid viewport");
        return;
      }
      glViewport(0, 0, w, h);

      glClearColor(0, 1, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawArraysInstanced(draw->mode, 0, count, inst);
    } break;

    case GRA_GL3_STEP_BLIT:
      dev_errf_(f, "BLIT is not implemented");
      return;

    default:
      assert(false);
      HEDLEY_UNREACHABLE();
    }
  }
  req->ok = true;
}


static void dev_init_lock_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"init lock cancelled\n");
    goto ABORT;
  }

  const bool pf = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso   = f->iso,
      .path  = (uint8_t*) GRA_GLFW_DEV_PATH,
      .len   = sizeof(GRA_GLFW_DEV_PATH)-1,
      .udata = k,
      .cb    = dev_init_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!pf)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GLFW device pathfind refusal\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void dev_init_pathfind_cb_(upd_pathfind_t* pf) {
  upd_file_lock_t* k   = pf->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_dev_t*   ctx = f->ctx;

  ctx->glfw = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(ctx->glfw == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"no GLFW device found\n");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(ctx->glfw->driver != &gra_glfw_dev)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GLFW device found but it's fake\n");
    goto ABORT;
  }
  upd_file_ref(ctx->glfw);

  const bool req = gra_glfw_lock_and_req_with_dup(&(gra_glfw_req_t) {
      .dev   = ctx->glfw,
      .type  = GRA_GLFW_REQ_GL3_INIT,
      .udata = k,
      .cb    = dev_init_gl3_cb_,
    });
  if (HEDLEY_UNLIKELY(!req)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GLFW_REQ_GL3_INIT is refused\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void dev_init_gl3_cb_(gra_glfw_req_t* req) {
  upd_file_lock_t* k   = req->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_dev_t*   ctx = f->ctx;

  ctx->gl = req->ok? req->win: NULL;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(ctx->gl == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"failed to create OpenGL window\n");
  }
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void dev_deinit_gl3_cb_(gra_glfw_req_t* req) {
  upd_iso_t* iso = req->udata;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}


static void dev_work_cb_(upd_iso_t* iso, void* udata) {
  gra_gl3_req_t* req = udata;
  upd_file_t*    f   = req->dev;
  gra_gl3_dev_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->error[0])) {
    upd_iso_msgf(iso, "OpenGL worker error: %s\n", ctx->error);
    ctx->error[0] = 0;
  }
  req->cb(req);
}
