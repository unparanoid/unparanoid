#pragma once

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hedley.h>
#include <luajit.h>
#include <lauxlib.h>
#include <msgpack.h>
#include <utf8.h>

#define UPD_EXTERNAL_DRIVER
#include <libupd.h>
#undef UPD_EXTERNAL_DRIVER

#include <libupd/array.h>
#include <libupd/buf.h>
#include <libupd/memory.h>
#include <libupd/msgpack.h>
#include <libupd/path.h>
#include <libupd/pathfind.h>


#define LJ_DEV_PATH "/sys/upd.luajit.dev"

#define LJ_INSTRUCTION_LIMIT 10000000  /* = 10^7 (10ms in 1 GHz clock) */


extern const upd_driver_t lj_dev;
extern const upd_driver_t lj_prog;
extern const upd_driver_t lj_stream;


typedef struct lj_dev_t    lj_dev_t;
typedef struct lj_prog_t   lj_prog_t;
typedef struct lj_stream_t lj_stream_t;

typedef struct lj_compile_t lj_compile_t;
typedef struct lj_promise_t lj_promise_t;


struct lj_dev_t {
  lua_State* L;
};


struct lj_prog_t {
  upd_file_t* dev;
  lua_State*  L;

  upd_file_watch_t watch;

  struct {
    int func;
  } registry;

  unsigned clean : 1;
};


typedef enum lj_stream_state_t {
  LJ_STREAM_EXITED  = 0x00,
  LJ_STREAM_ABORTED = 0x01,

  LJ_STREAM_RUNNING         = 0x10,
  LJ_STREAM_PENDING_TIMER   = 0x11,
  LJ_STREAM_PENDING_PROMISE = 0x12,
  LJ_STREAM_PENDING_INPUT   = 0x13,
} lj_stream_state_t;

struct lj_stream_t {
  lj_stream_state_t state;

  upd_file_watch_t watch;

  upd_file_t* dev;
  lua_State*  L;

  upd_file_t* prog;

  struct {
    int thread, func, ctx;
  } registry;

  upd_array_of(upd_file_t**)      files;
  upd_array_of(upd_file_lock_t**) locks;

  upd_buf_t in;
  upd_buf_t out;

  lj_promise_t* pending;
};


struct lj_compile_t {
  upd_file_t* prog;
  lua_State*  L;

  upd_file_lock_t lock;
  upd_req_t       req;

  int result;

  unsigned locked : 1;
  unsigned ok     : 1;

  void* udata;
  void
  (*cb)(
    lj_compile_t* cp);
};


struct lj_promise_t {
  upd_file_t* stream;

  struct {
    int self, result;
  } registry;

  unsigned error : 1;
  unsigned done  : 1;
};


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
bool
lj_stream_start(
  upd_file_t* f);

HEDLEY_NON_NULL(1)
bool
lj_stream_resume(
  upd_file_t* f);

HEDLEY_NON_NULL(1)
upd_file_t*
lj_stream_get(
  lua_State* L);


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
bool
lj_compile(
  lj_compile_t* cp);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
bool
lj_compile_with_dup(
  const lj_compile_t* src);


HEDLEY_NON_NULL(1)
void
lj_std_register(
  lua_State* L,
  upd_iso_t* iso);

HEDLEY_NON_NULL(1)
void
lj_ctx_create(
  lua_State*  L,
  upd_file_t* stf);


HEDLEY_NON_NULL(1)
void
lj_file_new(
  upd_file_t* stf,
  upd_file_t* f);

HEDLEY_NON_NULL(1)
void
lj_lock_new(
  upd_file_t*      stf,
  upd_file_lock_t* k);  /* heap address */

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
lj_promise_t*
lj_promise_new(
  upd_file_t* stf);

HEDLEY_NON_NULL(1)
int
lj_promise_push_result(
  lj_promise_t* pro);

HEDLEY_NON_NULL(1)
void
lj_promise_finalize(
  lj_promise_t* pro,
  bool          ok);


static inline bool lj_compile_with_dup(const lj_compile_t* src) {
  upd_iso_t* iso = src->prog->iso;

  lj_compile_t* cp = upd_iso_stack(iso, sizeof(*cp));
  if (HEDLEY_UNLIKELY(cp == NULL)) {
    return false;
  }
  *cp = *src;

  if (HEDLEY_UNLIKELY(!lj_compile(cp))) {
    upd_iso_unstack(iso, cp);
    return false;
  }
  return true;
}


static inline int lj_lua_immutable_newindex(lua_State* L) {
  return luaL_error(L, "immutable object cannot be modified");
}
