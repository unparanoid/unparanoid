#pragma once

/*
 * step    : an atomic OpenGL operation such as a single draw call
 * pipeline: be composed of 1 or more steps
 * pl      : pipeline
 * fb      : framebuffer
 * va      : vertex array
 */


#define GRA_GL3_STEP_DRAW_SHADER_MAX 16
#define GRA_GL3_STEP_DRAW_UNI_MAX    16
#define GRA_GL3_STEP_DRAW_IN_MAX     16

#define GRA_GL3_PL_IDENT_MAX     32
#define GRA_GL3_PL_FB_ATTACH_MAX 32
#define GRA_GL3_PL_VA_ATTACH_MAX 32


extern const upd_driver_t gra_gl3_buf_array;
extern const upd_driver_t gra_gl3_buf_element;
extern const upd_driver_t gra_gl3_dev;
extern const upd_driver_t gra_gl3_glsl_fragment;
extern const upd_driver_t gra_gl3_glsl_vertex;
extern const upd_driver_t gra_gl3_pl_def;
extern const upd_driver_t gra_gl3_tex_1d;
extern const upd_driver_t gra_gl3_tex_2d;
extern const upd_driver_t gra_gl3_tex_3d;


typedef struct gra_gl3_pl_value_t     gra_gl3_pl_value_t;
typedef struct gra_gl3_pl_var_t       gra_gl3_pl_var_t;
typedef struct gra_gl3_pl_out_t       gra_gl3_pl_out_t;
typedef struct gra_gl3_pl_rb_t        gra_gl3_pl_rb_t;
typedef struct gra_gl3_pl_fb_attach_t gra_gl3_pl_fb_attach_t;
typedef struct gra_gl3_pl_fb_t        gra_gl3_pl_fb_t;
typedef struct gra_gl3_pl_va_attach_t gra_gl3_pl_va_attach_t;
typedef struct gra_gl3_pl_va_t        gra_gl3_pl_va_t;
typedef struct gra_gl3_pl_t           gra_gl3_pl_t;

typedef struct gra_gl3_step_clear_t gra_gl3_step_clear_t;
typedef struct gra_gl3_step_draw_t  gra_gl3_step_draw_t;
typedef struct gra_gl3_step_blit_t  gra_gl3_step_blit_t;
typedef struct gra_gl3_step_t       gra_gl3_step_t;


typedef enum gra_gl3_pl_var_type_t {
  GRA_GL3_PL_VAR_NONE = 0x00,

  GRA_GL3_PL_VAR_RB = 0x01,
  GRA_GL3_PL_VAR_FB = 0x02,
  GRA_GL3_PL_VAR_VA = 0x03,

  GRA_GL3_PL_VAR_INTEGER = 0x04,
  GRA_GL3_PL_VAR_SCALAR  = 0x05,

  GRA_GL3_PL_VAR_VEC2 = 0x12,
  GRA_GL3_PL_VAR_VEC3 = 0x13,
  GRA_GL3_PL_VAR_VEC4 = 0x14,

  GRA_GL3_PL_VAR_MAT2 = 0x22,
  GRA_GL3_PL_VAR_MAT3 = 0x23,
  GRA_GL3_PL_VAR_MAT4 = 0x24,

  GRA_GL3_PL_VAR_TEX1 = 0x41,
  GRA_GL3_PL_VAR_TEX2 = 0x42,
  GRA_GL3_PL_VAR_TEX3 = 0x43,

  GRA_GL3_PL_VAR_BUF_ARRAY   = 0x81,
  GRA_GL3_PL_VAR_BUF_ELEMENT = 0x82,

  GRA_GL3_PL_VAR_DIM_MASK = 0x0f,
  GRA_GL3_PL_VAR_VEC_MASK = 0x10,
  GRA_GL3_PL_VAR_MAT_MASK = 0x20,
  GRA_GL3_PL_VAR_TEX_MASK = 0x40,
  GRA_GL3_PL_VAR_BUF_MASK = 0x80,
} gra_gl3_pl_var_type_t;

typedef enum gra_gl3_pl_value_type_t {
  GRA_GL3_PL_VALUE_NONE,
  GRA_GL3_PL_VALUE_REF,
  GRA_GL3_PL_VALUE_INTEGER,
  GRA_GL3_PL_VALUE_SCALAR,
} gra_gl3_pl_value_type_t;

struct gra_gl3_pl_value_t {
  gra_gl3_pl_value_type_t type;
  union {
    const gra_gl3_pl_var_t* var;
    intmax_t i;
    double   f;
  };
};

struct gra_gl3_pl_var_t {
  gra_gl3_pl_var_type_t type;

  unsigned in : 1;

  size_t offset;
  size_t index;

  uint8_t name[GRA_GL3_PL_IDENT_MAX];
};

struct gra_gl3_pl_out_t {
  const gra_gl3_pl_var_t* var;
  gra_gl3_pl_value_t reso[4];
};

struct gra_gl3_pl_rb_t {
  const gra_gl3_pl_var_t* var;

  GLenum fmt;
  gra_gl3_pl_value_t w;
  gra_gl3_pl_value_t h;
};

struct gra_gl3_pl_fb_attach_t {
  GLenum type;  /* GL_*_ATTACHMENT */
  const gra_gl3_pl_var_t* var;
};

struct gra_gl3_pl_fb_t {
  const gra_gl3_pl_var_t* var;

  gra_gl3_pl_fb_attach_t attach[GRA_GL3_PL_FB_ATTACH_MAX];
  size_t attachcnt;
};

struct gra_gl3_pl_va_attach_t {
  const gra_gl3_pl_var_t* buf;

  GLenum  type;
  uint8_t dim;

  gra_gl3_pl_value_t stride;
  gra_gl3_pl_value_t offset;
  gra_gl3_pl_value_t divisor;
};

struct gra_gl3_pl_va_t {
  const gra_gl3_pl_var_t* var;

  gra_gl3_pl_va_attach_t attach[GRA_GL3_PL_VA_ATTACH_MAX];
  size_t attachcnt;
};

struct gra_gl3_pl_t {
  gra_gl3_pl_var_t* var;
  size_t varcnt;

  gra_gl3_pl_out_t* out;
  size_t outcnt;

  gra_gl3_pl_rb_t* rb;
  size_t rbcnt;

  gra_gl3_pl_fb_t* fb;
  size_t fbcnt;

  gra_gl3_pl_va_t* va;
  size_t vacnt;

  gra_gl3_step_t* step;
  size_t stepcnt;

  size_t varbuflen;
};


struct gra_gl3_step_clear_t {
  GLbitfield bits;
  const gra_gl3_pl_fb_t* fb;
};

struct gra_gl3_step_draw_t {
  GLuint prog;
  GLenum mode;

  struct {
    struct {
      unsigned r : 1;
      unsigned g : 1;
      unsigned b : 1;
      unsigned a : 1;
    } mask;

    GLenum eq;
    GLenum src;
    GLenum dst;
  } blend;

  struct {
    bool   mask;
    GLenum func;
  } depth;

  gra_gl3_pl_value_t viewport[2];

  const gra_gl3_pl_va_t* va;
  const gra_gl3_pl_fb_t* fb;

  gra_gl3_pl_value_t count;
  gra_gl3_pl_value_t instance;

  size_t shadercnt;

  upd_file_t*        shader[GRA_GL3_STEP_DRAW_SHADER_MAX];
  gra_gl3_pl_value_t uni[GRA_GL3_STEP_DRAW_UNI_MAX];
};

struct gra_gl3_step_blit_t {
  const gra_gl3_pl_fb_t* in;
  const gra_gl3_pl_fb_t* out;

  struct {
    gra_gl3_pl_value_t x0, y0, x1, y1;
  } src, dst;

  GLbitfield mask;
};

typedef enum gra_gl3_step_type_t {
  GRA_GL3_STEP_NONE,
  GRA_GL3_STEP_CLEAR,
  GRA_GL3_STEP_DRAW,
  GRA_GL3_STEP_BLIT,
} gra_gl3_step_type_t;

struct gra_gl3_step_t {
  gra_gl3_step_type_t type;

  union {
    gra_gl3_step_clear_t clear;
    gra_gl3_step_draw_t  draw;
    gra_gl3_step_blit_t  blit;
  };
};


HEDLEY_NON_NULL(1, 2)
static inline
const gra_gl3_pl_var_t*
gra_gl3_pl_find_var(
  const gra_gl3_pl_t* pl,
  const uint8_t*      name,
  size_t              len);

HEDLEY_NON_NULL(1)
static inline
gra_gl3_pl_var_type_t
gra_gl3_pl_get_value_entity_type(
  const gra_gl3_pl_value_t* v);

HEDLEY_NON_NULL(1)
static inline
bool
gra_gl3_pl_get_value(
  const gra_gl3_pl_value_t* v,
  const uint8_t*            vbuf,
  int64_t*                  i,
  double*                   f);


static inline
gra_gl3_pl_var_type_t
gra_gl3_pl_var_type_unstringify(
  const uint8_t* str,
  size_t         len);

static inline
const upd_driver_t*
gra_gl3_get_driver_from_var_type(
  gra_gl3_pl_var_type_t type);


static inline const gra_gl3_pl_var_t* gra_gl3_pl_find_var(
    const gra_gl3_pl_t* pl, const uint8_t* name, size_t len) {
  if (HEDLEY_UNLIKELY(len >= GRA_GL3_PL_IDENT_MAX)) {
    return NULL;
  }
  for (size_t i = 0; i < pl->varcnt; ++i) {
    const gra_gl3_pl_var_t* v = &pl->var[i];
    if (HEDLEY_UNLIKELY(upd_streq_c(v->name, name, len))) {
      return v;
    }
  }
  return NULL;
}

static inline gra_gl3_pl_var_type_t gra_gl3_pl_get_value_entity_type(
    const gra_gl3_pl_value_t* v) {
  switch (v->type) {
  case GRA_GL3_PL_VALUE_REF:
    return v->var->type;
  case GRA_GL3_PL_VALUE_INTEGER:
    return GRA_GL3_PL_VAR_INTEGER;
  case GRA_GL3_PL_VALUE_SCALAR:
    return GRA_GL3_PL_VAR_SCALAR;
  default:
   return GRA_GL3_PL_VAR_NONE;
  }
}

static inline bool gra_gl3_pl_get_value(
    const gra_gl3_pl_value_t* v, const uint8_t* vbuf, intmax_t* i, double* f) {
  switch (v->type) {
  case GRA_GL3_PL_VALUE_REF:
    switch (v->var->type) {
    case GRA_GL3_PL_VAR_INTEGER:
      if (HEDLEY_UNLIKELY(i == NULL)) {
        return false;
      }
      *i = *(intmax_t*) (vbuf + v->var->offset);
      if (f) *f = *i;
      return true;

    case GRA_GL3_PL_VAR_SCALAR:
      if (HEDLEY_UNLIKELY(f == NULL)) {
        return false;
      }
      *f = *(double*) (vbuf + v->var->offset);
      if (i) *i = *f;
      return true;

    default:
      return false;
    }

  case GRA_GL3_PL_VALUE_INTEGER:
    if (HEDLEY_UNLIKELY(i == NULL)) {
      return false;
    }
    *i = v->i;
    if (f) *f = *i;
    return true;

  case GRA_GL3_PL_VALUE_SCALAR:
    if (HEDLEY_UNLIKELY(f == NULL)) {
      return false;
    }
    *f = v->f;
    if (i) *i = *f;
    return true;

  default:
    return false;
  }
}


static inline size_t gra_gl3_pl_sizeof_var(gra_gl3_pl_var_type_t t) {
  const uint8_t d = t & GRA_GL3_PL_VAR_DIM_MASK;

  switch (t) {
  case GRA_GL3_PL_VAR_VEC2:
  case GRA_GL3_PL_VAR_VEC3:
  case GRA_GL3_PL_VAR_VEC4:
    return sizeof(GLfloat)*d;

  case GRA_GL3_PL_VAR_INTEGER:
    return sizeof(GLint);

  case GRA_GL3_PL_VAR_SCALAR:
    return sizeof(GLfloat);

  case GRA_GL3_PL_VAR_MAT2:
  case GRA_GL3_PL_VAR_MAT3:
  case GRA_GL3_PL_VAR_MAT4:
    return sizeof(GLfloat)*d*d;

  case GRA_GL3_PL_VAR_RB:
  case GRA_GL3_PL_VAR_FB:
  case GRA_GL3_PL_VAR_VA:
  case GRA_GL3_PL_VAR_TEX1:
  case GRA_GL3_PL_VAR_TEX2:
  case GRA_GL3_PL_VAR_TEX3:
  case GRA_GL3_PL_VAR_BUF_ARRAY:
  case GRA_GL3_PL_VAR_BUF_ELEMENT:
    return sizeof(GLuint);

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }
}


static inline gra_gl3_pl_var_type_t gra_gl3_pl_var_type_unstringify(
    const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("integer", str, len)? GRA_GL3_PL_VAR_INTEGER:
    upd_strcaseq_c("scalar",  str, len)? GRA_GL3_PL_VAR_SCALAR:
    upd_strcaseq_c("vec2",    str, len)? GRA_GL3_PL_VAR_VEC2:
    upd_strcaseq_c("vec3",    str, len)? GRA_GL3_PL_VAR_VEC3:
    upd_strcaseq_c("vec4",    str, len)? GRA_GL3_PL_VAR_VEC4:
    upd_strcaseq_c("mat2",    str, len)? GRA_GL3_PL_VAR_MAT2:
    upd_strcaseq_c("mat3",    str, len)? GRA_GL3_PL_VAR_MAT3:
    upd_strcaseq_c("mat4",    str, len)? GRA_GL3_PL_VAR_MAT4:
    upd_strcaseq_c("tex1",    str, len)? GRA_GL3_PL_VAR_TEX1:
    upd_strcaseq_c("tex2",    str, len)? GRA_GL3_PL_VAR_TEX2:
    upd_strcaseq_c("tex3",    str, len)? GRA_GL3_PL_VAR_TEX3:
    upd_strcaseq_c("array",   str, len)? GRA_GL3_PL_VAR_BUF_ARRAY:
    upd_strcaseq_c("element", str, len)? GRA_GL3_PL_VAR_BUF_ELEMENT:
    GRA_GL3_PL_VAR_NONE;
}


static inline const upd_driver_t* gra_gl3_get_driver_from_var_type(
    gra_gl3_pl_var_type_t type) {
  switch (type) {
  case GRA_GL3_PL_VAR_TEX1:        return &gra_gl3_tex_1d;
  case GRA_GL3_PL_VAR_TEX2:        return &gra_gl3_tex_2d;
  case GRA_GL3_PL_VAR_TEX3:        return &gra_gl3_tex_3d;
  case GRA_GL3_PL_VAR_BUF_ARRAY:   return &gra_gl3_buf_array;
  case GRA_GL3_PL_VAR_BUF_ELEMENT: return &gra_gl3_buf_element;
  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }
}
