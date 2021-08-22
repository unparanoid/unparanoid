#pragma once

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>


#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <hedley.h>
#include <msgpack.h>


#define UPD_EXTERNAL_DRIVER
#include <libupd.h>
#undef UPD_EXTERNAL_DRIVER

#include <libupd/array.h>
#include <libupd/memory.h>
#include <libupd/msgpack.h>
#include <libupd/pathfind.h>
#include <libupd/proto.h>
#include <libupd/str.h>
#include <libupd/tensor.h>


static inline
uint32_t
gra_next_power2(
  uint32_t v);


#include "gl3_enum.h"
#include "gl3_pl.h"

#include "gl3.h"
#include "glfw.h"


static inline uint32_t gra_next_power2(uint32_t v) {
  /* https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2 */
  --v;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return ++v;
}
