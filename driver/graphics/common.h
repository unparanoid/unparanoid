#pragma once

#include <assert.h>
#include <setjmp.h>
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
#include <libupd/str.h>
#include <libupd/tensor.h>


#include "gl3_enum.h"
#include "gl3_pl.h"

#include "gl3.h"
#include "glfw.h"
