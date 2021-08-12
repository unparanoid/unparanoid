#include "common.h"

upd_external_t upd = {
  .ver = UPD_VER,
  .drivers = (const upd_driver_t*[]) {
    &gra_gl3_buf_array,
    &gra_gl3_buf_element,
    &gra_gl3_dev,
    &gra_gl3_glsl_fragment,
    &gra_gl3_glsl_vertex,
    &gra_gl3_pl_def,
    &gra_gl3_pl_lk,
    &gra_gl3_tex_1d,
    &gra_gl3_tex_2d,
    &gra_gl3_tex_3d,
    &gra_glfw_dev,
    NULL,
  },
};
