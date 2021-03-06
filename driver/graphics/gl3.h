#pragma once

#define GRA_GL3_DEV_PATH "/sys/upd.graphics.gl3.dev"


extern const upd_driver_t gra_gl3_buf_array;
extern const upd_driver_t gra_gl3_buf_element;
extern const upd_driver_t gra_gl3_dev;
extern const upd_driver_t gra_gl3_glsl_fragment;
extern const upd_driver_t gra_gl3_glsl_vertex;
extern const upd_driver_t gra_gl3_pl_def;
extern const upd_driver_t gra_gl3_pl_lk;
extern const upd_driver_t gra_gl3_tex_1d;
extern const upd_driver_t gra_gl3_tex_2d;
extern const upd_driver_t gra_gl3_tex_3d;
extern const upd_driver_t gra_gl3_view;


typedef struct gra_gl3_req_t   gra_gl3_req_t;
typedef struct gra_gl3_fetch_t gra_gl3_fetch_t;

typedef struct gra_gl3_buf_t    gra_gl3_buf_t;
typedef struct gra_gl3_dev_t    gra_gl3_dev_t;
typedef struct gra_gl3_glsl_t   gra_gl3_glsl_t;
typedef struct gra_gl3_pl_def_t gra_gl3_pl_def_t;
typedef struct gra_gl3_tex_t    gra_gl3_tex_t;
typedef struct gra_gl3_view_t   gra_gl3_view_t;


typedef enum gra_gl3_req_type_t {
  GRA_GL3_REQ_BUF_NEW,
  GRA_GL3_REQ_BUF_DELETE,
  GRA_GL3_REQ_BUF_ALLOC,
  GRA_GL3_REQ_BUF_MAP,
  GRA_GL3_REQ_BUF_UNMAP,
  GRA_GL3_REQ_BUF_MAP_PBO,
  GRA_GL3_REQ_BUF_UNMAP_PBO,

  GRA_GL3_REQ_TEX_NEW,
  GRA_GL3_REQ_TEX_DELETE,
  GRA_GL3_REQ_TEX_ALLOC,

  GRA_GL3_REQ_SHADER_RENEW,
  GRA_GL3_REQ_SHADER_DELETE,
  GRA_GL3_REQ_SHADER_COMPILE,

  GRA_GL3_REQ_PROG_CREATE,
  GRA_GL3_REQ_PROG_DELETE,
  GRA_GL3_REQ_PROG_ATTACH,
  GRA_GL3_REQ_PROG_LINK,

  GRA_GL3_REQ_PL_LINK,
  GRA_GL3_REQ_PL_UNLINK,
  GRA_GL3_REQ_PL_EXEC,
} gra_gl3_req_type_t;

struct gra_gl3_req_t {
  upd_file_t*     dev;
  upd_file_lock_t lock;

  gra_gl3_req_type_t type;

  union {
    struct {
      GLuint p[8];
      size_t n;
    } buf_multi;

    struct {
      GLuint id;
      GLenum target;
      GLenum usage;

      void*  data;
      size_t size;
    } buf_alloc;

    struct {
      GLuint id;
      GLenum target;
      GLenum mode;
      void*  data;
    } buf_map;

    struct {
      GLuint id;
      GLenum mode;
      GLuint tex;
      GLuint tex_target;
      GLenum fmt;
      GLenum type;

      void* data;

      uint32_t w, h, d;
      size_t   size;
    } buf_map_pbo;

    struct {
      GLuint id;

      GLenum target;
      GLenum fmt;
      GLenum type;

      uint32_t w, h, d;
      void*    data;
    } tex;

    struct {
      GLuint id;
      GLenum target;
    } shader;

    struct {
      GLuint id;
      GLuint shader;
    } prog;

    struct {
      const gra_gl3_pl_t* ptr;
      uint8_t*            varbuf;
    } pl;
  };

  unsigned ok : 1;

  void* udata;
  void
  (*cb)(
    gra_gl3_req_t* req);
};


typedef enum gra_gl3_fetch_type_t {
  GRA_GL3_FETCH_GLSL,
  GRA_GL3_FETCH_PL,
} gra_gl3_fetch_type_t;

struct gra_gl3_fetch_t {
  upd_file_t*     file;
  upd_file_lock_t lock;

  gra_gl3_fetch_type_t type;

  union {
    GLuint glsl;
    const gra_gl3_pl_t* pl;
  };

  unsigned ok : 1;

  void* udata;
  void
  (*cb)(
    gra_gl3_fetch_t* fe);
};


struct gra_gl3_buf_t {
  upd_file_t* gl;

  GLenum target;
  GLuint id;

  upd_tensor_type_t type;
  uint32_t          reso;

  struct {
    GLenum mode;
    size_t size;
    void*  data;

    size_t refcnt;
    upd_array_of(upd_req_t*) pending;
  } map;

  unsigned broken : 1;
};

struct gra_gl3_dev_t {
  upd_file_t* glfw;
  GLFWwindow* gl;

  uint8_t error[256];
};

struct gra_gl3_glsl_t {
  upd_file_t* gl;

  GLenum target;
  GLuint id;

  upd_file_t*      bin;
  upd_file_watch_t watch;

  upd_array_of(gra_gl3_fetch_t*) pending;

  unsigned broken : 1;
  unsigned clean  : 1;
};

struct gra_gl3_pl_def_t {
  upd_file_t* gl;

  upd_file_watch_t watch;

  gra_gl3_pl_t* pl;
  upd_array_of(upd_file_watch_t*) shwatch;

  upd_array_of(gra_gl3_fetch_t) pending;

  unsigned clean : 1;
};

struct gra_gl3_tex_t {
  upd_file_t* gl;

  GLenum  target;
  uint8_t rank;

  GLuint id;
  GLuint pbo;

  GLenum fmt;

  uint32_t ch, w, h, d;

  struct {
    GLenum mode;
    GLenum type;
    void*  data;
    size_t size;

    size_t refcnt;
    upd_array_of(upd_req_t*) pending;
  } map;

  unsigned broken : 1;
};


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
bool
gra_gl3_req(
  gra_gl3_req_t* req);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
bool
gra_gl3_glsl_fetch(
  gra_gl3_fetch_t* fe);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
bool
gra_gl3_pl_def_fetch(
  gra_gl3_fetch_t* fe);


HEDLEY_NON_NULL(1)
static inline
void
gra_gl3_buf_set_metadata(
  upd_file_t*       buf,
  upd_tensor_type_t type,
  size_t            reso);

/* 'req' must be a req with type GRA_GL3_REQ_TEX_ALLOC. */
HEDLEY_NON_NULL(1)
static inline
void
gra_gl3_tex_set_metadata(
  upd_file_t*          tex,
  const gra_gl3_req_t* req);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
bool
gra_gl3_dev_make_ctx_current(
  upd_file_t*  gl,
  const char** err);


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
bool
gra_gl3_lock_and_req(
  gra_gl3_req_t* req);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
gra_gl3_req_t*
gra_gl3_lock_and_req_with_dup(
  const gra_gl3_req_t* src);


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
bool
gra_gl3_lock_and_fetch(
  gra_gl3_fetch_t* fe);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
gra_gl3_fetch_t*
gra_gl3_lock_and_fetch_with_dup(
  const gra_gl3_fetch_t* src);


static inline
void
gra_gl3_lock_and_req_lock_cb_(
  upd_file_lock_t* k);

static inline
void
gra_gl3_lock_and_fetch_lock_cb_(
  upd_file_lock_t* k);


static inline void gra_gl3_buf_set_metadata(
    upd_file_t* buf, const upd_tensor_type_t type, size_t reso) {
  gra_gl3_buf_t* ctx = buf->ctx;
  ctx->type = type;
  ctx->reso = reso;
}

static inline void gra_gl3_tex_set_metadata(
    upd_file_t* tex, const gra_gl3_req_t* req) {
  gra_gl3_tex_t* ctx = tex->ctx;
  assert(ctx->target == req->tex.target);

  ctx->w = 1, ctx->h = 1, ctx->d = 1;

  ctx->ch  = gra_gl3_color_fmt_to_dim(req->tex.fmt);
  ctx->fmt = req->tex.fmt;

  switch (req->tex.target) {
  case GL_TEXTURE_3D:
    ctx->d = req->tex.d;
    HEDLEY_FALL_THROUGH;
  case GL_TEXTURE_2D:
    ctx->h = req->tex.h;
    HEDLEY_FALL_THROUGH;
  case GL_TEXTURE_1D:
    ctx->w = req->tex.w;
  }
}

static inline bool gra_gl3_dev_make_ctx_current(upd_file_t* gl, const char** err) {
  gra_gl3_dev_t* ctx = gl->ctx;
  if (HEDLEY_UNLIKELY(ctx->gl == NULL)) {
    if (err) *err = "OpenGL context is not generated";
    return false;
  }
  glfwMakeContextCurrent(ctx->gl);
  return !glfwGetError(err);
}


static inline bool gra_gl3_lock_and_req(gra_gl3_req_t* req) {
  req->lock = (upd_file_lock_t) {
    .file  = req->dev,
    .ex    = true,
    .udata = req,
    .cb    = gra_gl3_lock_and_req_lock_cb_,
  };
  return upd_file_lock(&req->lock);
}

static inline gra_gl3_req_t* gra_gl3_lock_and_req_with_dup(
    const gra_gl3_req_t* src) {
  upd_file_t* f   = src->dev;
  upd_iso_t*  iso = f->iso;

  gra_gl3_req_t* req = upd_iso_stack(iso, sizeof(*req));
  if (HEDLEY_UNLIKELY(req == NULL)) {
    return NULL;
  }
  *req = *src;

  if (HEDLEY_UNLIKELY(!gra_gl3_lock_and_req(req))) {
    upd_iso_unstack(iso, req);
    return NULL;
  }
  return req;
}


static inline bool gra_gl3_lock_and_fetch(gra_gl3_fetch_t* fe) {
  fe->lock = (upd_file_lock_t) {
    .file  = fe->file,
    .udata = fe,
    .cb    = gra_gl3_lock_and_fetch_lock_cb_,
  };
  return upd_file_lock(&fe->lock);
}

static inline gra_gl3_fetch_t* gra_gl3_lock_and_fetch_with_dup(
    const gra_gl3_fetch_t* src) {
  upd_file_t* f   = src->file;
  upd_iso_t*  iso = f->iso;

  gra_gl3_fetch_t* fe = upd_iso_stack(iso, sizeof(*fe));
  if (HEDLEY_UNLIKELY(fe == NULL)) {
    return NULL;
  }
  *fe = *src;

  if (HEDLEY_UNLIKELY(!gra_gl3_lock_and_fetch(fe))) {
    upd_iso_unstack(iso, fe);
    return NULL;
  }
  return fe;
}


static inline void gra_gl3_lock_and_req_lock_cb_(upd_file_lock_t* k) {
  gra_gl3_req_t* req = k->udata;

  if (HEDLEY_UNLIKELY(!k->ok || !gra_gl3_req(req))) {
    req->ok = false;
    req->cb(req);
  }
}

static inline void gra_gl3_lock_and_fetch_lock_cb_(upd_file_lock_t* k) {
  gra_gl3_fetch_t* fe = k->udata;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    goto ABORT;
  }

  bool ok = false;
  switch (fe->type) {
  case GRA_GL3_FETCH_GLSL:
    ok = gra_gl3_glsl_fetch(fe);
    break;
  case GRA_GL3_FETCH_PL:
    ok = gra_gl3_pl_def_fetch(fe);
    break;
  default:
    assert(false);
  }
  if (HEDLEY_UNLIKELY(!ok)) {
    goto ABORT;
  }
  return;

ABORT:
  fe->ok = false;
  fe->cb(fe);
}
