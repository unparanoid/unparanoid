#pragma once


static inline GLenum gra_gl3_rank_to_tex_target(uint8_t rank) {
  return
    rank == 2? GL_TEXTURE_1D:
    rank == 3? GL_TEXTURE_2D:
    rank == 4? GL_TEXTURE_3D: 0;
}

static inline GLenum gra_gl3_dim_to_color_fmt(uint32_t dim) {
  return
    dim == 1? GL_R:
    dim == 2? GL_RG:
    dim == 3? GL_RGB:
    dim == 4? GL_RGBA: 0;
}


static inline bool gra_gl3_enum_unstringify_attachment(
    GLenum* v, const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("color0",  str, len)? (*v = GL_COLOR_ATTACHMENT0,  true):
    upd_strcaseq_c("color1",  str, len)? (*v = GL_COLOR_ATTACHMENT1,  true):
    upd_strcaseq_c("color2",  str, len)? (*v = GL_COLOR_ATTACHMENT2,  true):
    upd_strcaseq_c("color3",  str, len)? (*v = GL_COLOR_ATTACHMENT3,  true):
    upd_strcaseq_c("color4",  str, len)? (*v = GL_COLOR_ATTACHMENT4,  true):
    upd_strcaseq_c("color5",  str, len)? (*v = GL_COLOR_ATTACHMENT5,  true):
    upd_strcaseq_c("color6",  str, len)? (*v = GL_COLOR_ATTACHMENT6,  true):
    upd_strcaseq_c("color7",  str, len)? (*v = GL_COLOR_ATTACHMENT7,  true):
    upd_strcaseq_c("depth",   str, len)? (*v = GL_DEPTH_ATTACHMENT,   true):
    upd_strcaseq_c("stencil", str, len)? (*v = GL_STENCIL_ATTACHMENT, true):
    false;
}

static inline bool gra_gl3_enum_unstringify_format(
    GLenum* v, const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("r8",       str, len)? (*v = GL_R8,                 true):
    upd_strcaseq_c("rg8",      str, len)? (*v = GL_RG8,                true):
    upd_strcaseq_c("rgb8",     str, len)? (*v = GL_RGB8,               true):
    upd_strcaseq_c("rgba4",    str, len)? (*v = GL_RGBA4,              true):
    upd_strcaseq_c("rgba8",    str, len)? (*v = GL_RGBA8,              true):
    upd_strcaseq_c("depth16",  str, len)? (*v = GL_DEPTH_COMPONENT16,  true):
    upd_strcaseq_c("depth24",  str, len)? (*v = GL_DEPTH_COMPONENT24,  true):
    upd_strcaseq_c("depth32f", str, len)? (*v = GL_DEPTH_COMPONENT32F, true):
    upd_strcaseq_c("stencil",  str, len)? (*v = GL_STENCIL_INDEX8,     true):
    false;
}

static inline bool gra_gl3_enum_unstringify_draw_mode(
    GLenum* v, const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("triangle_strip", str, len)? (*v = GL_TRIANGLE_STRIP, true):
    upd_strcaseq_c("triangle_fan",   str, len)? (*v = GL_TRIANGLE_FAN,   true):
    upd_strcaseq_c("triangles",      str, len)? (*v = GL_TRIANGLES,      true):
    false;
}

static inline bool gra_gl3_enum_unstringify_buf_type(
    GLenum* v, const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("f32", str, len)? (*v = GL_FLOAT, true):
    upd_strcaseq_c("u8",  str, len)? (*v = GL_FLOAT, true):
    upd_strcaseq_c("u16", str, len)? (*v = GL_FLOAT, true):
    false;
}

static inline bool gra_gl3_enum_unstringify_blend_factor(
    GLenum* v, const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("zero",                str, len)? (*v = GL_ZERO,                true):
    upd_strcaseq_c("one",                 str, len)? (*v = GL_ONE,                 true):
    upd_strcaseq_c("src_color",           str, len)? (*v = GL_SRC_COLOR,           true):
    upd_strcaseq_c("one_minus_src_color", str, len)? (*v = GL_ONE_MINUS_SRC_COLOR, true):
    upd_strcaseq_c("dst_color",           str, len)? (*v = GL_DST_COLOR,           true):
    upd_strcaseq_c("one_minus_dst_color", str, len)? (*v = GL_ONE_MINUS_DST_COLOR, true):
    upd_strcaseq_c("src_alpha",           str, len)? (*v = GL_SRC_ALPHA,           true):
    upd_strcaseq_c("one_minus_src_alpha", str, len)? (*v = GL_ONE_MINUS_SRC_ALPHA, true):
    upd_strcaseq_c("dst_alpha",           str, len)? (*v = GL_DST_ALPHA,           true):
    upd_strcaseq_c("one_minus_dst_alpha", str, len)? (*v = GL_ONE_MINUS_DST_ALPHA, true):
    false;
}

static inline bool gra_gl3_enum_unstringify_blend_eq(
    GLenum* v, const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("add",              str, len)? (*v = GL_FUNC_ADD,              true):
    upd_strcaseq_c("subtract",         str, len)? (*v = GL_FUNC_SUBTRACT,         true):
    upd_strcaseq_c("reverse_subtract", str, len)? (*v = GL_FUNC_REVERSE_SUBTRACT, true):
    upd_strcaseq_c("min",              str, len)? (*v = GL_MIN,                   true):
    upd_strcaseq_c("max",              str, len)? (*v = GL_MAX,                   true):
    false;
}

static inline bool gra_gl3_enum_unstringify_depth_func(
    GLenum* v, const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("never",    str, len)? (*v = GL_NEVER,    true):
    upd_strcaseq_c("less",     str, len)? (*v = GL_LESS,     true):
    upd_strcaseq_c("equal",    str, len)? (*v = GL_EQUAL,    true):
    upd_strcaseq_c("lequal",   str, len)? (*v = GL_LEQUAL,   true):
    upd_strcaseq_c("greater",  str, len)? (*v = GL_GREATER,  true):
    upd_strcaseq_c("notequal", str, len)? (*v = GL_NOTEQUAL, true):
    upd_strcaseq_c("gequal",   str, len)? (*v = GL_GEQUAL,   true):
    upd_strcaseq_c("always",   str, len)? (*v = GL_ALWAYS,   true):
    false;
}

static inline bool gra_gl3_enum_unstringify_clear_bit(
    GLenum* v, const uint8_t* str, size_t len) {
  return
    upd_strcaseq_c("color",   str, len)? (*v = GL_COLOR_BUFFER_BIT,   true):
    upd_strcaseq_c("depth",   str, len)? (*v = GL_DEPTH_BUFFER_BIT,   true):
    upd_strcaseq_c("stencil", str, len)? (*v = GL_STENCIL_BUFFER_BIT, true):
    false;
}
