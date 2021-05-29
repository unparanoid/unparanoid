#include "common.h"


#define SCRIPT_MAX_ (1024*1024*8)

#define DEV_PATH_ "/sys/upd.lua.dev"

#define SANDBOX_INSTRUCTION_LIMIT_ 10000000  /* = 10^7 (10ms in 1 GHz clock) */


typedef struct prog_t_    prog_t_;
typedef struct stream_t_  stream_t_;
typedef struct promise_t_ promise_t_;


struct prog_t_ {
  upd_file_t* file;
  upd_file_t* dev;

  upd_file_watch_t watch;

  size_t   size;
  uint8_t* buf;
  uv_file  fd;

  struct {
    int func;
  } registry;

  unsigned clean    : 1;
  unsigned compiled : 1;
};


typedef enum stream_state_t_ {
  STREAM_EXITED_  = 0x00,
  STREAM_ABORTED_ = 0x10,

  STREAM_RUNNING_         = 0x01,
  STREAM_PENDING_TIMER_   = 0x11,
  STREAM_PENDING_PROMISE_ = 0x21,
  STREAM_PENDING_INPUT_   = 0x31,

  STREAM_FLAG_ALIVE_ = 0x01,
} stream_state_t_;

struct stream_t_ {
  uv_timer_t timer;

  lua_State* lua;

  /* strong reference */
  upd_file_t* file;
  upd_file_t* dev;
  upd_file_t* prog;

  struct {
    int thread;
    int func;
    int ctx;
  } registry;

  upd_array_t files;

  upd_buf_t in;
  upd_buf_t out;

  promise_t_* pending;

  stream_state_t_ state;
};


typedef enum promise_type_t_ {
  PROMISE_REQ_,
  PROMISE_PATHFIND_,
  PROMISE_REQUIRE_,
} promise_type_t_;

struct promise_t_ {
  union {
    upd_req_t          req;
    upd_req_pathfind_t pf;
  };
  promise_type_t_ type;

  upd_file_t* file;

  struct {
    int self;
    int result;
  } registry;

  unsigned error : 1;
  unsigned done  : 1;
};


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

const upd_driver_t upd_driver_lua_dev = {
  .name = (uint8_t*) "upd.lua.dev",
  .cats = (upd_req_cat_t[]) {
    0,
  },
  .init   = dev_init_,
  .deinit = dev_deinit_,
  .handle = dev_handle_,
};


static
bool
prog_init_(
  upd_file_t* f);

static
void
prog_deinit_(
  upd_file_t* f);

static
bool
prog_handle_(
  upd_req_t* req);

static
bool
prog_compile_(
  upd_file_t* f);

static
upd_file_t*
prog_exec_(
  upd_file_t* f);

const upd_driver_t upd_driver_lua = {
  .name = (uint8_t*) "upd.lua",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_BIN,
    UPD_REQ_PROG,
    0,
  },
  .flags  = {
    .npoll = true,
  },
  .init   = prog_init_,
  .deinit = prog_deinit_,
  .handle = prog_handle_,
};


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

static const upd_driver_t stream_ = {
  .name = (uint8_t*) "upd.lua.stream",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_STREAM,
    0,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


static
void
logf_(
  upd_file_t* f,
  const char* fmt,
  ...);


static
void
prog_lock_for_pathfind_cb_(
  upd_file_lock_t* lock);

static
void
prog_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
prog_watch_cb_(
  upd_file_watch_t* w);

static
void
prog_stat_cb_(
  uv_fs_t* fsreq);

static
void
prog_open_cb_(
  uv_fs_t* fsreq);

static
void
prog_read_cb_(
  uv_fs_t* fsreq);

static
void
prog_close_cb_(
  uv_fs_t* fsreq);

static
void
prog_exec_after_compile_cb_(
  upd_req_t* req);


static
void
stream_lua_hook_cb_(
  lua_State* state,
  lua_Debug* dbg);

static
void
stream_timer_cb_(
  uv_timer_t* timer);

static
void
stream_close_cb_(
  uv_handle_t* handle);


static
int
lua_immutable_newindex_(
  lua_State* lua);


static
void
lua_lib_register_class_(
  lua_State* lua);


static
void
lua_context_create_(
  lua_State*  lua,
  upd_file_t* stf);

static
upd_file_t*
lua_context_get_file_(
  lua_State* lua);


static
void
lua_file_register_class_(
  lua_State* lua);

static
void
lua_file_new_(
  lua_State*  lua,
  upd_file_t* file);


static
void
lua_req_register_class_(
  lua_State* lua,
  upd_iso_t* iso);


static
void
lua_promise_register_class_(
  lua_State* lua);

static
promise_t_*
lua_promise_new_(
  lua_State* lua,
  size_t     add);

static
int
lua_promise_push_result_(
  promise_t_* pro,
  lua_State*  lua);

static
void
lua_promise_finalize_(
  promise_t_* pro,
  bool        ok);


static
void
lua_promise_req_cb_(
  upd_req_t* req);

static
void
lua_promise_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
lua_promise_require_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
lua_promise_require_compile_cb_(
  upd_req_t* req);


static bool dev_init_(upd_file_t* f) {
  lua_State* lua = luaL_newstate();
  if (HEDLEY_UNLIKELY(lua == NULL)) {
    return false;
  }
  lua_lib_register_class_(lua);
  lua_file_register_class_(lua);
  lua_req_register_class_(lua, f->iso);
  lua_promise_register_class_(lua);

  f->ctx = lua;
  return true;
}

static void dev_deinit_(upd_file_t* f) {
  lua_State* lua = f->ctx;
  lua_close(lua);
}

static bool dev_handle_(upd_req_t* req) {
  req->result = UPD_REQ_INVALID;
  return false;
}


static bool prog_init_(upd_file_t* f) {
  prog_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (prog_t_) {
    .file = f,
    .watch = {
      .file = f,
      .cb   = prog_watch_cb_,
    },
  };
  f->ctx = ctx;

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    return false;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file = f,
      .ex   = true,
      .cb   = prog_lock_for_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    return false;
  }
  return true;
}

static void prog_deinit_(upd_file_t* f) {
  prog_t_*    ctx = f->ctx;
  upd_file_t* dev = ctx->dev;
  lua_State*  lua = dev->ctx;

  upd_file_unwatch(&ctx->watch);

  if (HEDLEY_UNLIKELY(ctx->compiled)) {
    luaL_unref(lua, LUA_REGISTRYINDEX, ctx->registry.func);
  }
  upd_free(&ctx);

  if (HEDLEY_LIKELY(dev)) {
    upd_file_unref(dev);
  }
}

static bool prog_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  prog_t_*    ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  switch (req->type) {
  case UPD_REQ_BIN_ACCESS:
    req->bin.access = (upd_req_bin_access_t) {
      .write = !f->npath,
    };
    break;
  case UPD_REQ_BIN_WRITE: {
    if (HEDLEY_UNLIKELY(f->npath || req->bin.rw.offset)) {
      req->result = UPD_REQ_INVALID;
      return false;
    }
    ctx->buf  = req->bin.rw.buf;
    ctx->size = req->bin.rw.size;
    if (HEDLEY_UNLIKELY(!prog_compile_(f))) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
  } break;

  case UPD_REQ_PROG_ACCESS:
    req->prog.access = (upd_req_prog_access_t) {
      .compile = !!f->npath,
      .exec    = true,
    };
    break;

  case UPD_REQ_PROG_COMPILE: {
    if (HEDLEY_UNLIKELY(!f->npath)) {
      req->result = UPD_REQ_INVALID;
      return false;
    }
    if (HEDLEY_LIKELY(ctx->clean && ctx->compiled)) {
      break;
    }

    uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
    if (HEDLEY_UNLIKELY(fsreq == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    *fsreq = (uv_fs_t) { .data = req, };

    upd_file_ref(f);
    const bool stat =
      0 <= uv_fs_stat(&iso->loop, fsreq, (char*) f->npath, prog_stat_cb_);
    if (HEDLEY_UNLIKELY(!stat)) {
      upd_file_unref(f);
      req->result = UPD_REQ_ABORTED;
      return false;
    }
  } return true;

  case UPD_REQ_PROG_EXEC: {
    req->prog.exec = NULL;
    if (HEDLEY_LIKELY(ctx->clean && ctx->compiled)) {
      upd_file_t* stf = prog_exec_(f);
      req->prog.exec = stf;
      req->result    = UPD_REQ_OK;
      req->cb(req);
      upd_file_unref(stf);
      return true;
    }
    if (HEDLEY_UNLIKELY(!f->npath)) {
      req->result = UPD_REQ_INVALID;
      return false;
    }
    const bool compile = upd_req_with_dup(&(upd_req_t) {
        .file  = f,
        .type  = UPD_REQ_PROG_COMPILE,
        .udata = req,
        .cb    = prog_exec_after_compile_cb_,
      });
    if (HEDLEY_UNLIKELY(!compile)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}

static bool prog_compile_(upd_file_t* f) {
  prog_t_*    ctx = f->ctx;
  upd_file_t* dev = ctx->dev;
  lua_State*  lua = dev->ctx;

  if (HEDLEY_LIKELY(ctx->compiled)) {
    luaL_unref(lua, LUA_REGISTRYINDEX, ctx->registry.func);
  }
  ctx->compiled = false;

  const int load = luaL_loadbuffer(
    lua, (char*) ctx->buf, ctx->size, (char*) f->npath);

  switch (load) {
  case LUA_ERRSYNTAX:
    logf_(f, "lua parser syntax error");
    return false;
  case LUA_ERRMEM:
    logf_(f, "lua parser allocation failure");
    return false;
  }
  ctx->compiled      = true;
  ctx->clean         = true;
  ctx->registry.func = luaL_ref(lua, LUA_REGISTRYINDEX);
  return true;
}

static upd_file_t* prog_exec_(upd_file_t* prf) {
  upd_iso_t*  iso = prf->iso;
  prog_t_*    ctx = prf->ctx;
  upd_file_t* dev = ctx->dev;
  lua_State*  lua = dev->ctx;

  upd_file_t* stf = upd_file_new(iso, &stream_);
  if (HEDLEY_UNLIKELY(prf == NULL)) {
    return NULL;
  }

  stream_t_* st = stf->ctx;
  st->file = stf;
  st->dev  = dev;
  st->prog = prf;

  upd_file_ref(st->dev);
  upd_file_ref(st->prog);

  lua_State* th = lua_newthread(lua);
  if (HEDLEY_UNLIKELY(th == NULL)) {
    logf_(prf, "failed to create new thread");
    upd_file_unref(stf);
    return NULL;
  }
  if (HEDLEY_UNLIKELY(0 > uv_timer_start(&st->timer, stream_timer_cb_, 0, 0))) {
    logf_(prf, "failed to start runner");
    upd_file_unref(stf);
    return NULL;
  }
  st->lua = th;

  lua_pushthread(th);
  st->registry.thread = luaL_ref(th, LUA_REGISTRYINDEX);

  lua_sethook(th,
    stream_lua_hook_cb_, LUA_MASKCOUNT, SANDBOX_INSTRUCTION_LIMIT_);

  lua_createtable(th, 0, 0);
  {
    lua_createtable(th, 0, 0);
    {
      lua_createtable(th, 0, 0);
      {
        lua_context_create_(th, stf);
        lua_setfield(th, -2, "Context");

        lua_getfield(th, LUA_REGISTRYINDEX, "Lua");
        lua_setfield(th, -2, "Lua");

        lua_getfield(th, LUA_REGISTRYINDEX, "File");
        lua_setfield(th, -2, "File");

        lua_getfield(th, LUA_REGISTRYINDEX, "Req");
        lua_setfield(th, -2, "Req");
      }
      lua_setfield(th, -2, "__index");

      lua_pushcfunction(th, lua_immutable_newindex_);
      lua_setfield(th, -2, "__newindex");
    }
    lua_setmetatable(th, -2);
  }
  st->registry.ctx = luaL_ref(th, LUA_REGISTRYINDEX);

  lua_pushthread(th);
  lua_rawgeti(th, LUA_REGISTRYINDEX, st->registry.ctx);
  lua_setfenv(th, -2);
  lua_pop(th, 1);

  lua_rawgeti(th, LUA_REGISTRYINDEX, ctx->registry.func);
  st->registry.func = luaL_ref(th, LUA_REGISTRYINDEX);
  return stf;
}


static bool stream_init_(upd_file_t* f) {
  stream_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (stream_t_) {
    .file = f,
  };

  if (HEDLEY_UNLIKELY(0 > uv_timer_init(&f->iso->loop, &ctx->timer))) {
    upd_free(&ctx);
    return false;
  }
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;

  for (size_t i = 0; i < ctx->files.n; ++i) {
    upd_file_t** udata = ctx->files.p[i];
    upd_file_unref(*udata);
    *udata = NULL;
  }
  upd_array_clear(&ctx->files);

  upd_buf_clear(&ctx->in);
  upd_buf_clear(&ctx->out);

  if (HEDLEY_LIKELY(ctx->dev)) {
    upd_file_unref(ctx->dev);
  }
  if (HEDLEY_LIKELY(ctx->prog)) {
    upd_file_unref(ctx->prog);
  }
  if (HEDLEY_LIKELY(ctx->lua)) {
    luaL_unref(ctx->lua, LUA_REGISTRYINDEX, ctx->registry.ctx);
    luaL_unref(ctx->lua, LUA_REGISTRYINDEX, ctx->registry.func);

    luaL_unref(ctx->lua, LUA_REGISTRYINDEX, ctx->registry.thread);
    lua_gc(ctx->lua, LUA_GCCOLLECT, 0);
  }
  uv_timer_stop(&ctx->timer);
  uv_close((uv_handle_t*) &ctx->timer, stream_close_cb_);
}

static bool stream_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  stream_t_*  ctx = f->ctx;

  switch (req->type) {
  case UPD_REQ_STREAM_ACCESS:
    req->stream.access = (upd_req_stream_access_t) {
      .input  = true,
      .output = true,
    };
    break;

  case UPD_REQ_STREAM_INPUT: {
    const upd_req_stream_io_t io = req->stream.io;
    req->stream.io.size = 0;
    if (HEDLEY_UNLIKELY(!io.size)) {
      break;
    }
    if (HEDLEY_UNLIKELY(!(ctx->state & STREAM_FLAG_ALIVE_))) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    if (HEDLEY_UNLIKELY(!upd_buf_append(&ctx->in, io.buf, io.size))) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    if (HEDLEY_UNLIKELY(ctx->state == STREAM_PENDING_INPUT_)) {
      const bool start =
        0 <= uv_timer_start(&ctx->timer, stream_timer_cb_, 0, 0);
      if (HEDLEY_UNLIKELY(!start)) {
        assert(false);
      }
    }
    req->stream.io.size = io.size;
  } break;

  case UPD_REQ_STREAM_OUTPUT: {
    const bool alive = ctx->state & STREAM_FLAG_ALIVE_;
    if (HEDLEY_UNLIKELY(ctx->out.size == 0 && !alive)) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    req->stream.io = (upd_req_stream_io_t) {
      .buf  = ctx->out.ptr,
      .size = ctx->out.size,
    };
    req->result = UPD_REQ_OK;
    req->cb(req);
    upd_buf_clear(&ctx->out);
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}


static void logf_(upd_file_t* f, const char* fmt, ...) {
  upd_iso_t* iso = f->iso;

  upd_iso_msgf(iso, "upd.lua error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(iso, fmt, args);
  va_end(args);

  upd_iso_msgf(iso, " (%s)\n", f->npath);
}


static void prog_lock_for_pathfind_cb_(upd_file_lock_t* lock) {
  upd_file_t* f   = lock->file;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }

  const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso   = f->iso,
      .path  = (uint8_t*) DEV_PATH_,
      .len   = utf8size_lazy(DEV_PATH_),
      .udata = lock,
      .cb    = prog_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!pf)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
}

static void prog_pathfind_cb_(upd_req_pathfind_t* pf) {
  upd_file_lock_t* lock = pf->udata;
  upd_file_t*      f    = lock->file;
  prog_t_*         ctx  = f->ctx;
  upd_iso_t*       iso  = f->iso;

  upd_file_t* dev = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(dev == NULL)) {
    logf_(f, "'"DEV_PATH_"' is not found");
    goto EXIT;
  }

  if (HEDLEY_UNLIKELY(dev->driver != &upd_driver_lua_dev)) {
    logf_(f, "'"DEV_PATH_"' is not upd.lua.dev");
    goto EXIT;
  }

  upd_file_ref(dev);
  ctx->dev = dev;

EXIT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
}

static void prog_watch_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->file;
  prog_t_*    ctx = f->ctx;

  if (HEDLEY_UNLIKELY(w->event == UPD_FILE_UPDATE_N)) {
    ctx->clean = false;
  }
}

static void prog_stat_cb_(uv_fs_t* fsreq) {
  upd_req_t*  req = fsreq->data;
  upd_file_t* f   = req->file;
  prog_t_*    ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result != 0)) {
    logf_(f, "stat failure");
    req->result = UPD_REQ_ABORTED;
    goto ABORT;
  }

  ctx->size = fsreq->statbuf.st_size;
  if (HEDLEY_UNLIKELY(ctx->size > SCRIPT_MAX_)) {
    logf_(f, "too huge script (%zu exceeds %zu)", ctx->size, SCRIPT_MAX_);
    req->result = UPD_REQ_ABORTED;
    goto ABORT;
  }

  const bool open = 0 <= uv_fs_open(
    &iso->loop, fsreq, (char*) f->npath, O_RDONLY, 0, prog_open_cb_);
  if (HEDLEY_UNLIKELY(!open)) {
    logf_(f, "open failure");
    req->result = UPD_REQ_ABORTED;
    goto ABORT;
  }
  return;

ABORT:
  upd_iso_unstack(iso, fsreq);
  req->cb(req);
  upd_file_unref(f);
}

static void prog_open_cb_(uv_fs_t* fsreq) {
  upd_req_t*  req = fsreq->data;
  upd_file_t* f   = req->file;
  prog_t_*    ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    logf_(f, "open failure");
    req->result = UPD_REQ_ABORTED;
    goto ABORT;
  }
  ctx->fd = result;

  ctx->buf = upd_iso_stack(iso, ctx->size);
  if (HEDLEY_UNLIKELY(ctx->buf == NULL)) {
    logf_(f, "read buffer allocation failure");
    req->result = UPD_REQ_NOMEM;
    goto ABORT;
  }

  uv_buf_t buf = uv_buf_init((char*) ctx->buf, ctx->size);

  const bool read =
    0 <= uv_fs_read(&iso->loop, fsreq, ctx->fd, &buf, 1, 0, prog_read_cb_);
  if (HEDLEY_UNLIKELY(!read)) {
    upd_iso_unstack(iso, ctx->buf);
    logf_(f, "read failure");
    req->result = UPD_REQ_ABORTED;
    goto ABORT;
  }
  return;

ABORT:
  upd_iso_unstack(iso, fsreq);
  req->cb(req);
  upd_file_unref(f);
}

static void prog_read_cb_(uv_fs_t* fsreq) {
  upd_req_t*  req = fsreq->data;
  upd_file_t* f   = req->file;
  prog_t_*    ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    logf_(f, "read failure");
    req->result = UPD_REQ_ABORTED;
    goto EXIT;
  }
  req->result = prog_compile_(f)? UPD_REQ_OK: UPD_REQ_ABORTED;

EXIT:
  upd_iso_unstack(iso, ctx->buf);
  req->cb(req);

  fsreq->data = f;
  if (HEDLEY_UNLIKELY(0 > uv_fs_close(&iso->loop, fsreq, ctx->fd, prog_close_cb_))) {
    upd_file_unref(f);
  }
}

static void prog_close_cb_(uv_fs_t* fsreq) {
  upd_file_t* f = fsreq->data;
  uv_fs_req_cleanup(fsreq);
  upd_iso_unstack(f->iso, fsreq);
  upd_file_unref(f);
}

static void prog_exec_after_compile_cb_(upd_req_t* req) {
  upd_file_t* f      = req->file;
  prog_t_*    ctx    = f->ctx;
  upd_iso_t*  iso    = f->iso;
  upd_req_t*  origin = req->udata;
  upd_iso_unstack(iso, req);

  upd_file_t* stf = ctx->compiled? prog_exec_(f): NULL;

  origin->prog.exec = stf;
  origin->result    = stf? UPD_REQ_OK: UPD_REQ_ABORTED;
  origin->cb(origin);

  if (HEDLEY_LIKELY(stf)) {
    upd_file_unref(stf);
  }
}


static void stream_lua_destroy_hook_cb_(lua_State* lua, lua_Debug* dbg) {
  (void) dbg;
  luaL_error(lua, "reaches instruction limit %d", SANDBOX_INSTRUCTION_LIMIT_);
}
static void stream_lua_hook_cb_(lua_State* lua, lua_Debug* dbg) {
  (void) dbg;
  lua_sethook(lua, stream_lua_destroy_hook_cb_, LUA_MASKLINE, 0);
}

static void stream_timer_cb_(uv_timer_t* timer) {
  stream_t_*  ctx = (void*) timer;
  upd_file_t* stf = ctx->file;
  upd_file_t* prf = ctx->prog;
  lua_State*  lua = ctx->lua;

  lua_rawgeti(lua, LUA_REGISTRYINDEX, ctx->registry.func);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, ctx->registry.ctx);
  lua_setfenv(lua, -2);

  int args = 0;
  switch ((intmax_t) ctx->state) {
  case STREAM_PENDING_INPUT_:
    lua_pushlstring(lua, (char*) ctx->in.ptr, ctx->in.size);
    args = 1;
    upd_buf_clear(&ctx->in);
    break;
  case STREAM_PENDING_PROMISE_:
    args = lua_promise_push_result_(ctx->pending, lua);
    break;
  }
  ctx->state = STREAM_RUNNING_;

  const int err = lua_resume(lua, args);
  switch (err) {
  case 0:
    ctx->state = STREAM_EXITED_;

    for (size_t i = 0; i < ctx->files.n; ++i) {
      upd_file_t** udata = ctx->files.p[i];
      upd_file_unref(*udata);
      *udata = NULL;
    }
    upd_array_clear(&ctx->files);
    break;
  case LUA_YIELD:
    break;
  case LUA_ERRRUN:
  case LUA_ERRMEM:
  case LUA_ERRERR: {
    lua_Debug dbg;
    lua_getstack(lua, 0, &dbg);
    lua_getinfo(lua, "Sl", &dbg);
    logf_(prf, "runtime error (%s) (%s:%d)",
      lua_tostring(lua, -1), dbg.source, dbg.currentline);
    ctx->state = STREAM_ABORTED_;
  } break;
  }
  lua_settop(lua, 0);

  /*  upd_file_trigger could destroy the file unexpectedly
   * without this reference... */
  upd_file_ref(stf);
  {
    if (HEDLEY_UNLIKELY(ctx->out.size)) {
      upd_file_trigger(stf, UPD_FILE_UPDATE);
    }

    switch ((intmax_t) ctx->state) {
    case STREAM_RUNNING_: {
      const bool start = 0 <= uv_timer_start(&ctx->timer, stream_timer_cb_, 0, 0);
      if (HEDLEY_UNLIKELY(!start)) {
        assert(false);
      }
    } break;
    case STREAM_EXITED_:
    case STREAM_ABORTED_:
      upd_file_trigger(stf, UPD_FILE_UPDATE);
      break;
    }
  }
  upd_file_unref(stf);
}

static void stream_close_cb_(uv_handle_t* handle) {
  stream_t_* ctx = (void*) handle;
  upd_free(&ctx);
}


static int lua_immutable_newindex_(lua_State* lua) {
  return luaL_error(lua, "immutable object cannot be modified");
}


static int lua_lib_can_require_(lua_State* lua) {
  upd_file_t* f = *(void**) luaL_checkudata(lua, 1, "File");
  lua_pushboolean(lua, f->driver == &upd_driver_lua);
  return 1;
}
static int lua_lib_require_(lua_State* lua) {
  size_t len;
  const char* path = luaL_checklstring(lua, 1, &len);

  promise_t_* pro = lua_promise_new_(lua, len+1);
  pro->type = PROMISE_REQUIRE_;

  const int index = lua_gettop(lua);

  pro->pf = (upd_req_pathfind_t) {
    .iso   = pro->file->iso,
    .path  = (uint8_t*) (pro+1),
    .len   = len,
    .udata = pro+1,
    .cb    = lua_promise_require_pathfind_cb_,
  };
  utf8ncpy(pro+1, path, len);

  upd_req_pathfind(&pro->pf);
  lua_pushvalue(lua, index);
  return 1;
}
static int lua_lib_set_metatable_(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  luaL_checktype(lua, 2, LUA_TTABLE);
  lua_setmetatable(lua, -2);
  return 1;
}
static int lua_lib_pairs_internal_(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);

  if (HEDLEY_LIKELY(lua_next(lua, 1))) {
    return 2;
  }
  return 0;
}
static int lua_lib_pairs_(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  lua_pushcfunction(lua, lua_lib_pairs_internal_);
  lua_pushvalue(lua, 1);
  lua_pushnil(lua);
  return 3;
}
static void lua_lib_register_class_(lua_State* lua) {
  lua_newuserdata(lua, 0);
  {
    lua_createtable(lua, 0, 0);
    {
      lua_createtable(lua, 0, 0);
      {
        lua_pushcfunction(lua, lua_lib_can_require_);
        lua_setfield(lua, -2, "canRequire");

        lua_pushcfunction(lua, lua_lib_pairs_);
        lua_setfield(lua, -2, "pairs");

        lua_pushcfunction(lua, lua_lib_require_);
        lua_setfield(lua, -2, "require");

        lua_pushcfunction(lua, lua_lib_set_metatable_);
        lua_setfield(lua, -2, "setMetatable");
      }
      lua_setfield(lua, -2, "__index");
    }
    lua_setmetatable(lua, -2);
  }
  lua_setfield(lua, LUA_REGISTRYINDEX, "Lua");
}


static int lua_context_msg_(lua_State* lua) {
  upd_iso_t* iso = lua_touserdata(lua, lua_upvalueindex(1));

  const size_t n = lua_gettop(lua);
  for (size_t i = 1; i <= n; ++i) {
    size_t len;
    const char* s = luaL_checklstring(lua, i, &len);

    upd_iso_msg(iso, (uint8_t*) s, len);
  }
  return 0;
}
static int lua_context_recv_(lua_State* lua) {
  upd_file_t* f   = lua_touserdata(lua, lua_upvalueindex(1));
  stream_t_*  ctx = f->ctx;

  lua_pushlstring(lua, (char*) ctx->in.ptr, ctx->in.size);
  upd_buf_clear(&ctx->in);
  return 1;
}
static int lua_context_recv_blocked_(lua_State* lua) {
  upd_file_t* f   = lua_touserdata(lua, lua_upvalueindex(1));
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_LIKELY(!ctx->in.size)) {
    ctx->state = STREAM_PENDING_INPUT_;
    return lua_yield(lua, 0);
  }

  lua_pushlstring(lua, (char*) ctx->in.ptr, ctx->in.size);
  upd_buf_clear(&ctx->in);
  return 1;
}
static int lua_context_send_(lua_State* lua) {
  upd_file_t* f   = lua_touserdata(lua, lua_upvalueindex(1));
  stream_t_*  ctx = f->ctx;

  const size_t n = lua_gettop(lua);
  for (size_t i = 1; i <= n; ++i) {
    size_t len;
    const char* s = luaL_checklstring(lua, i, &len);

    if (HEDLEY_UNLIKELY(len && !upd_buf_append(&ctx->out, (uint8_t*) s, len))) {
      return luaL_error(lua, "buffer allocation failure");
    }
  }
  return 0;
}
static int lua_context_sleep_(lua_State* lua) {
  upd_file_t* f   = lua_touserdata(lua, lua_upvalueindex(1));
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 1)) {
    return luaL_error(lua, "Context.sleep() takes 1 arg");
  }

  ctx->state = STREAM_PENDING_TIMER_;

  const lua_Integer t = luaL_checkinteger(lua, 1);
  if (HEDLEY_UNLIKELY(t < 0)) {
    return luaL_error(lua, "negative duration: %"PRIiMAX"", (intmax_t) t);
  }

  const bool start = 0 <= uv_timer_start(&ctx->timer, stream_timer_cb_, t, 0);
  if (HEDLEY_UNLIKELY(!start)) {
    assert(false);
  }
  return lua_yield(lua, 0);
}
static void lua_context_create_(lua_State* lua, upd_file_t* f) {
  *(void**) lua_newuserdata(lua, sizeof(f)) = f;

  lua_createtable(lua, 0, 0);
  {
    lua_createtable(lua, 0, 0);
    {
      lua_pushlightuserdata(lua, f->iso);
      lua_pushcclosure(lua, lua_context_msg_, 1);
      lua_setfield(lua, -2, "msg");

      lua_pushlightuserdata(lua, f);
      lua_pushcclosure(lua, lua_context_sleep_, 1);
      lua_setfield(lua, -2, "sleep");

      lua_pushlightuserdata(lua, f);
      lua_pushcclosure(lua, lua_context_recv_, 1);
      lua_setfield(lua, -2, "recv");

      lua_pushlightuserdata(lua, f);
      lua_pushcclosure(lua, lua_context_recv_blocked_, 1);
      lua_setfield(lua, -2, "recvBlocked");

      lua_pushlightuserdata(lua, f);
      lua_pushcclosure(lua, lua_context_send_, 1);
      lua_setfield(lua, -2, "send");
    }
    lua_setfield(lua, -2, "__index");
  }
  lua_setmetatable(lua, -2);
}

static upd_file_t* lua_context_get_file_(lua_State* lua) {
  lua_pushthread(lua);
  lua_getfenv(lua, -1);
  lua_getfield(lua, -1, "Context");
  upd_file_t* f = *(void**) lua_touserdata(lua, -1);
  lua_pop(lua, 3);
  return f;
}


static int lua_file_index_(lua_State* lua) {
  upd_file_t* f = *(void**) lua_touserdata(lua, 1);

  const char* key = luaL_checkstring(lua, 2);
  if (HEDLEY_UNLIKELY(utf8cmp(key, "npath") == 0)) {
    lua_pushstring(lua, (char*) f->npath);
    return 1;
  }
  return luaL_error(lua, "unknown field %s", key);
}
static int lua_file_gc_(lua_State* lua) {
  upd_file_t** udata = lua_touserdata(lua, 1);
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return 0;
  }
  upd_file_t* stf = lua_context_get_file_(lua);
  stream_t_*  ctx = stf->ctx;
  upd_array_find_and_remove(&ctx->files, udata);
  upd_file_unref(*udata);
  return 0;
}
static void lua_file_register_class_(lua_State* lua) {
  lua_createtable(lua, 0, 0);
  {
    lua_pushcfunction(lua, lua_file_index_);
    lua_setfield(lua, -2, "__index");

    lua_pushcfunction(lua, lua_file_gc_);
    lua_setfield(lua, -2, "__gc");
  }
  lua_setfield(lua, LUA_REGISTRYINDEX, "File");
}

static void lua_file_new_(lua_State* lua, upd_file_t* f) {
  upd_file_t* stf = lua_context_get_file_(lua);
  stream_t_*  ctx = stf->ctx;

  if (HEDLEY_UNLIKELY(f == NULL)) {
    lua_pushnil(lua);
    return;
  }

  upd_file_t** udata = lua_newuserdata(lua, sizeof(f));
  *udata = f;

  lua_getfield(lua, LUA_REGISTRYINDEX, "File");
  lua_setmetatable(lua, -2);

  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->files, udata, SIZE_MAX))) {
    luaL_error(lua, "file list insertion failure");
  }
  upd_file_ref(f);
}


static int lua_req_pathfind_(lua_State* lua) {
  upd_iso_t* iso = lua_touserdata(lua, lua_upvalueindex(1));

  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 1)) {
    return luaL_error(lua, "Req.pathfind() takes one arg");
  }

  size_t len;
  const char* path = luaL_checklstring(lua, 1, &len);

  promise_t_* pro = lua_promise_new_(lua, len);
  pro->type = PROMISE_PATHFIND_;

  pro->pf = (upd_req_pathfind_t) {
    .iso  = iso,
    .path = utf8ncpy(pro+1, path, len),
    .len  = len,
    .cb   = lua_promise_pathfind_cb_,
  };
  upd_req_pathfind(&pro->pf);
  return 1;
}
static int lua_req_emit_no_args_(lua_State* lua) {
  const upd_req_type_t type = lua_tointeger(lua, lua_upvalueindex(1));

  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 1)) {
    return luaL_error(lua, "invalid args");
  }
  upd_file_t* f = *(void**) luaL_checkudata(lua, 1, "File");

  promise_t_* pro = lua_promise_new_(lua, 0);
  const int index = lua_gettop(lua);

  pro->type = PROMISE_REQ_;
  pro->req  = (upd_req_t) {
    .file = f,
    .type = type,
    .cb   = lua_promise_req_cb_,
  };

  if (HEDLEY_UNLIKELY(!upd_req(&pro->req))) {
    lua_promise_finalize_(pro, false);
  }
  lua_pushvalue(lua, index);
  return 1;
}
static int lua_req_dir_with_entry_name_(lua_State* lua) {
  const upd_req_type_t type = lua_tointeger(lua, lua_upvalueindex(1));

  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 2)) {
    return luaL_error(lua, "invalid args");
  }
  upd_file_t* f = *(void**) luaL_checkudata(lua, 1, "File");

  size_t len;
  const char* name = luaL_checklstring(lua, 2, &len);

  promise_t_* pro = lua_promise_new_(lua, len);
  const int index = lua_gettop(lua);

  pro->type = PROMISE_REQ_;
  pro->req  = (upd_req_t) {
    .file = f,
    .type = type,
    .dir  = { .entry = {
      .name = utf8ncpy(pro+1, name, len),
      .len  = len,
    }, },
    .cb = lua_promise_req_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&pro->req))) {
    lua_promise_finalize_(pro, false);
  }
  lua_pushvalue(lua, index);
  return 1;
}
static int lua_req_dir_add_(lua_State* lua) {
  const upd_req_type_t type = lua_tointeger(lua, lua_upvalueindex(1));

  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 3)) {
    return luaL_error(lua, "invalid args");
  }
  upd_file_t* f = *(void**) luaL_checkudata(lua, 1, "File");
  upd_file_t* c = *(void**) luaL_checkudata(lua, 2, "File");

  size_t len;
  const char* name = luaL_checklstring(lua, 3, &len);

  promise_t_* pro = lua_promise_new_(lua, len);
  const int index = lua_gettop(lua);

  pro->type = PROMISE_REQ_;
  pro->req  = (upd_req_t) {
    .file = f,
    .type = type,
    .dir  = { .entry = {
      .name = utf8ncpy(pro+1, name, len),
      .len  = len,
      .file = c,
    }, },
    .cb = lua_promise_req_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&pro->req))) {
    lua_promise_finalize_(pro, false);
  }
  lua_pushvalue(lua, index);
  return 1;
}
static int lua_req_dir_rm_(lua_State* lua) {
  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 2)) {
    return luaL_error(lua, "invalid args");
  }
  upd_file_t* f = *(void**) luaL_checkudata(lua, 1, "File");

  size_t      len   = 0;
  const char* name  = NULL;
  upd_file_t* child = NULL;
  if (lua_isstring(lua, 2)) {
    name = luaL_checklstring(lua, 2, &len);
  } else {
    child = luaL_checkudata(lua, 2, "File");
  }

  promise_t_* pro = lua_promise_new_(lua, len);
  const int index = lua_gettop(lua);

  pro->type = PROMISE_REQ_;
  pro->req  = (upd_req_t) {
    .file = f,
    .type = UPD_REQ_DIR_RM,
    .dir  = { .entry = {
      .name = utf8ncpy(pro+1, name, len),
      .len  = len,
      .file = child,
    }, },
    .cb = lua_promise_req_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&pro->req))) {
    lua_promise_finalize_(pro, false);
  }
  lua_pushvalue(lua, index);
  return 1;
}
static int lua_req_bin_read_(lua_State* lua) {
  const size_t n = lua_gettop(lua);
  if (HEDLEY_UNLIKELY(n > 3 || (n == 3 && !lua_isnumber(lua, 3)))) {
    return luaL_error(lua, "invalid args");
  }
  upd_file_t* f = *(void**) luaL_checkudata(lua, 1, "File");

  const lua_Integer size = luaL_checkinteger(lua, 2);
  if (HEDLEY_UNLIKELY(size < 0)) {
    return luaL_error(lua, "size must be 0 or more");
  }

  const lua_Integer offset = lua_tointeger(lua, 3);
  if (HEDLEY_UNLIKELY(offset < 0)) {
    return luaL_error(lua, "offset must be 0 or more");
  }

  promise_t_* pro = lua_promise_new_(lua, 0);
  const int index = lua_gettop(lua);

  pro->type = PROMISE_REQ_;
  pro->req  = (upd_req_t) {
    .file = f,
    .type = UPD_REQ_BIN_READ,
    .bin = { .rw = {
      .offset = offset,
      .size   = size,
    }, },
    .cb = lua_promise_req_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&pro->req))) {
    lua_promise_finalize_(pro, false);
  }
  lua_pushvalue(lua, index);
  return 1;
}
static int lua_req_bin_write_(lua_State* lua) {
  const size_t n = lua_gettop(lua);
  if (HEDLEY_UNLIKELY(n > 3 || (n == 3 && !lua_isnumber(lua, 3)))) {
    return luaL_error(lua, "invalid args");
  }
  upd_file_t* f = *(void**) luaL_checkudata(lua, 1, "File");

  size_t len;
  const char* data = luaL_checklstring(lua, 2, &len);

  const lua_Integer offset = lua_tointeger(lua, 3);
  if (HEDLEY_UNLIKELY(offset < 0)) {
    return luaL_error(lua, "offset must be 0 or more");
  }

  promise_t_* pro = lua_promise_new_(lua, len);
  const int index = lua_gettop(lua);

  pro->type = PROMISE_REQ_;
  pro->req  = (upd_req_t) {
    .file = f,
    .type = UPD_REQ_BIN_WRITE,
    .bin = { .rw = {
      .buf    = memcpy(pro+1, data, len),
      .offset = offset,
      .size   = len,
    }, },
    .cb = lua_promise_req_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&pro->req))) {
    lua_promise_finalize_(pro, false);
  }
  lua_pushvalue(lua, index);
  return 1;
}
static int lua_req_stream_input_(lua_State* lua) {
  return luaL_error(lua, "not implemented");
}
static int lua_req_stream_output_(lua_State* lua) {
  return luaL_error(lua, "not implemented");
}
static int lua_req_tensor_alloc_(lua_State* lua) {
  return luaL_error(lua, "not implemented");
}
static int lua_req_tensor_meta_(lua_State* lua) {
  return luaL_error(lua, "not implemented");
}
static void lua_req_register_class_(lua_State* lua, upd_iso_t* iso) {
  lua_createtable(lua, 0, 0);
  {
    lua_createtable(lua, 0, 0);
    {
      lua_createtable(lua, 0, 0);
      {
        lua_pushlightuserdata(lua, iso);
        lua_pushcclosure(lua, lua_req_pathfind_, 1);
        lua_setfield(lua, -2, "pathfind");

        lua_createtable(lua, 0, 0);
        {
          lua_pushinteger(lua, UPD_REQ_DIR_ACCESS);
          lua_pushcclosure(lua, lua_req_emit_no_args_, 1);
          lua_setfield(lua, -2, "access");

          lua_pushinteger(lua, UPD_REQ_DIR_LIST);
          lua_pushcclosure(lua, lua_req_emit_no_args_, 1);
          lua_setfield(lua, -2, "list");

          lua_pushinteger(lua, UPD_REQ_DIR_FIND);
          lua_pushcclosure(lua, lua_req_dir_with_entry_name_, 1);
          lua_setfield(lua, -2, "find");

          lua_pushcfunction(lua, lua_req_dir_add_);
          lua_setfield(lua, -2, "add");

          lua_pushinteger(lua, UPD_REQ_DIR_NEW);
          lua_pushcclosure(lua, lua_req_dir_with_entry_name_, 1);
          lua_setfield(lua, -2, "new");

          lua_pushinteger(lua, UPD_REQ_DIR_NEWDIR);
          lua_pushcclosure(lua, lua_req_dir_with_entry_name_, 1);
          lua_setfield(lua, -2, "newdir");

          lua_pushcfunction(lua, lua_req_dir_rm_);
          lua_setfield(lua, -2, "rm");
        }
        lua_setfield(lua, -2, "dir");

        lua_createtable(lua, 0, 0);
        {
          lua_pushinteger(lua, UPD_REQ_BIN_ACCESS);
          lua_pushcclosure(lua, lua_req_emit_no_args_, 1);
          lua_setfield(lua, -2, "access");

          lua_pushcfunction(lua, lua_req_bin_read_);
          lua_setfield(lua, -2, "read");

          lua_pushcfunction(lua, lua_req_bin_write_);
          lua_setfield(lua, -2, "write");
        }
        lua_setfield(lua, -2, "bin");

        lua_createtable(lua, 0, 0);
        {
          lua_pushinteger(lua, UPD_REQ_PROG_ACCESS);
          lua_pushcclosure(lua, lua_req_emit_no_args_, 1);
          lua_setfield(lua, -2, "access");

          lua_pushinteger(lua, UPD_REQ_PROG_COMPILE);
          lua_pushcclosure(lua, lua_req_emit_no_args_, 1);
          lua_setfield(lua, -2, "compile");

          lua_pushinteger(lua, UPD_REQ_PROG_EXEC);
          lua_pushcclosure(lua, lua_req_emit_no_args_, 1);
          lua_setfield(lua, -2, "exec");
        }
        lua_setfield(lua, -2, "prog");

        lua_createtable(lua, 0, 0);
        {
          lua_pushinteger(lua, UPD_REQ_STREAM_ACCESS);
          lua_pushcclosure(lua, lua_req_emit_no_args_, 1);
          lua_setfield(lua, -2, "access");

          lua_pushcfunction(lua, lua_req_stream_input_);
          lua_setfield(lua, -2, "input");

          lua_pushcfunction(lua, lua_req_stream_output_);
          lua_setfield(lua, -2, "output");
        }
        lua_setfield(lua, -2, "stream");

        lua_createtable(lua, 0, 0);
        {
          lua_pushinteger(lua, UPD_REQ_TENSOR_ACCESS);
          lua_pushcclosure(lua, lua_req_emit_no_args_, 1);
          lua_setfield(lua, -2, "access");

          lua_pushcfunction(lua, lua_req_tensor_alloc_);
          lua_setfield(lua, -2, "alloc");

          lua_pushcfunction(lua, lua_req_tensor_meta_);
          lua_setfield(lua, -2, "meta");
        }
        lua_setfield(lua, -2, "tensor");
      }
      lua_setfield(lua, -2, "__index");

      lua_pushcfunction(lua, lua_immutable_newindex_);
      lua_setfield(lua, -2, "__newindex");
    }
    lua_setmetatable(lua, -2);
  }
  lua_setfield(lua, LUA_REGISTRYINDEX, "Req");
}


static int lua_promise_result_(lua_State* lua) {
  promise_t_* pro = lua_touserdata(lua, lua_upvalueindex(1));
  if (HEDLEY_UNLIKELY(lua_gettop(lua) > 0)) {
    return luaL_error(lua, "Promise.result() takes no args");
  }
  return lua_promise_push_result_(pro, lua);
}
static int lua_promise_await_(lua_State* lua) {
  promise_t_* pro = lua_touserdata(lua, lua_upvalueindex(1));
  upd_file_t* f   = pro->file;
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(lua_gettop(lua) > 0)) {
    return luaL_error(lua, "Promise.await() takes no args");
  }
  if (!pro->done) {
    ctx->state   = STREAM_PENDING_PROMISE_;
    ctx->pending = pro;
    return lua_yield(lua, 0);
  }
  return lua_promise_result_(lua);
}
static int lua_promise_index_(lua_State* lua) {
  promise_t_* pro = lua_touserdata(lua, 1);

  const char* key = luaL_checkstring(lua, 2);
  if (HEDLEY_UNLIKELY(utf8cmp(key, "result") == 0)) {
    lua_pushlightuserdata(lua, pro);
    lua_pushcclosure(lua, lua_promise_result_, 1);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "await") == 0)) {
    lua_pushlightuserdata(lua, pro);
    lua_pushcclosure(lua, lua_promise_await_, 1);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "done") == 0)) {
    lua_pushboolean(lua, pro->done);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "error") == 0)) {
    lua_pushboolean(lua, pro->error);
    return 1;
  }
  return luaL_error(lua, "unknown field %s", key);
}
static int lua_promise_gc_(lua_State* lua) {
  promise_t_* pro = lua_touserdata(lua, 1);
  luaL_unref(lua, LUA_REGISTRYINDEX, pro->registry.result);
  return 0;
}
static void lua_promise_register_class_(lua_State* lua) {
  lua_createtable(lua, 0, 0);
  {
    lua_pushcfunction(lua, lua_promise_index_);
    lua_setfield(lua, -2, "__index");

    lua_pushcfunction(lua, lua_promise_gc_);
    lua_setfield(lua, -2, "__gc");
  }
  lua_setfield(lua, LUA_REGISTRYINDEX, "Promise");
}

static promise_t_* lua_promise_new_(lua_State* lua, size_t add) {
  promise_t_* pro = lua_newuserdata(lua, sizeof(*pro)+add);
  lua_getfield(lua, LUA_REGISTRYINDEX, "Promise");
  lua_setmetatable(lua, -2);

  *pro = (promise_t_) {
    .file = lua_context_get_file_(lua),
    .registry = {
      .result = LUA_REFNIL,
    },
  };
  upd_file_ref(pro->file);

  lua_pushvalue(lua, -1);
  pro->registry.self = luaL_ref(lua, LUA_REGISTRYINDEX);
  return pro;
}

static int lua_promise_push_result_(promise_t_* pro, lua_State* lua) {
  lua_rawgeti(lua, LUA_REGISTRYINDEX, pro->registry.result);
  if (HEDLEY_UNLIKELY(lua_istable(lua, -1))) {
    for (int i = 1; ; ++i) {
      lua_rawgeti(lua, -i, i);
      if (HEDLEY_UNLIKELY(lua_isnil(lua, -1))) {
        lua_pop(lua, 1);
        return i-1;
      }
    }
  }
  return 1;
}

static void lua_promise_finalize_(promise_t_* pro, bool ok) {
  upd_file_t* f   = pro->file;
  stream_t_*  ctx = f->ctx;

  assert(!pro->done);
  pro->done  = true;
  pro->error = !ok;

  if (HEDLEY_UNLIKELY(pro == ctx->pending)) {
    const bool start =
      0 <= uv_timer_start(&ctx->timer, stream_timer_cb_, 0, 0);
    if (HEDLEY_UNLIKELY(!start)) {
      assert(false);
    }
  }
  luaL_unref(ctx->lua, LUA_REGISTRYINDEX, pro->registry.self);
  pro->registry.self = LUA_REFNIL;
  upd_file_unref(f);
}


static void lua_promise_req_cb_(upd_req_t* req) {
  promise_t_* pro = (void*) req;
  upd_file_t* stf = pro->file;
  stream_t_*  ctx = stf->ctx;
  lua_State*  lua = ctx->lua;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    lua_promise_finalize_(pro, false);
    return;
  }

  switch (req->type) {
  case UPD_REQ_DIR_ACCESS:
    lua_createtable(lua, 0, 0);
    lua_createtable(lua, 0, 0);
    {
      lua_pushboolean(lua, req->dir.access.list);
      lua_setfield(lua, -2, "list");

      lua_pushboolean(lua, req->dir.access.find);
      lua_setfield(lua, -2, "find");

      lua_pushboolean(lua, req->dir.access.add);
      lua_setfield(lua, -2, "add");

      lua_pushboolean(lua, req->dir.access.new);
      lua_setfield(lua, -2, "new");

      lua_pushboolean(lua, req->dir.access.new);
      lua_setfield(lua, -2, "newdir");

      lua_pushboolean(lua, req->dir.access.rm);
      lua_setfield(lua, -2, "rm");
    }
    lua_rawseti(lua, -2, 1);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_DIR_LIST:
    lua_createtable(lua, 0, 0);
    lua_createtable(lua, 0, 0);
    for (size_t i = 0; i < req->dir.entries.n; ++i) {
      upd_req_dir_entry_t* e = req->dir.entries.p[i];
      lua_pushlstring(lua, (char*) e->name, e->len);
      lua_file_new_(lua, e->file);
      lua_settable(lua, -3);
    }
    lua_rawseti(lua, -2, 1);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_DIR_FIND:
  case UPD_REQ_DIR_ADD:
  case UPD_REQ_DIR_NEW:
  case UPD_REQ_DIR_NEWDIR:
  case UPD_REQ_DIR_RM:
    lua_file_new_(lua, req->dir.entry.file);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_BIN_ACCESS:
    lua_createtable(lua, 0, 0);
    lua_createtable(lua, 0, 0);
    {
      lua_pushboolean(lua, req->bin.access.read);
      lua_setfield(lua, -2, "read");

      lua_pushboolean(lua, req->bin.access.write);
      lua_setfield(lua, -2, "write");
    }
    lua_rawseti(lua, -2, 1);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_BIN_READ:
    lua_createtable(lua, 0, 0);
    {
      lua_pushlstring(lua, (char*) req->bin.rw.buf, req->bin.rw.size);
      lua_rawseti(lua, -2, 1);

      lua_pushinteger(lua, req->bin.rw.offset);
      lua_rawseti(lua, -2, 2);
    }
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_BIN_WRITE:
    lua_pushinteger(lua, req->bin.rw.size);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_PROG_ACCESS:
    lua_createtable(lua, 0, 0);
    lua_createtable(lua, 0, 0);
    {
      lua_pushboolean(lua, req->prog.access.compile);
      lua_setfield(lua, -2, "compile");

      lua_pushboolean(lua, req->prog.access.exec);
      lua_setfield(lua, -2, "exec");
    }
    lua_rawseti(lua, -2, 1);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_PROG_COMPILE:
    break;

  case UPD_REQ_PROG_EXEC:
    lua_file_new_(lua, req->prog.exec);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_STREAM_ACCESS:
    lua_createtable(lua, 0, 0);
    lua_createtable(lua, 0, 0);
    {
      lua_pushboolean(lua, req->stream.access.input);
      lua_setfield(lua, -2, "input");

      lua_pushboolean(lua, req->stream.access.output);
      lua_setfield(lua, -2, "output");
    }
    lua_rawseti(lua, -2, 1);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_STREAM_INPUT:
    lua_pushinteger(lua, req->stream.io.size);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_STREAM_OUTPUT:
    lua_pushlstring(lua, (char*) req->stream.io.buf, req->stream.io.size);
    pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
    break;

  default:
    luaL_error(lua, "not implemented request type");
  }
  lua_promise_finalize_(pro, true);
}

static void lua_promise_pathfind_cb_(upd_req_pathfind_t* pf) {
  promise_t_* pro = (void*) pf;
  upd_file_t* stf = pro->file;
  stream_t_*  ctx = stf->ctx;
  lua_State*  lua = ctx->lua;

  lua_createtable(lua, 0, 0);
  if (HEDLEY_UNLIKELY(pf->len)) {
    lua_pushnil(lua);
    lua_rawseti(lua, -2, 1);

    lua_pushlstring(lua, (char*) pf->path, pf->len);
    lua_rawseti(lua, -2, 2);

    lua_file_new_(lua, pf->base);
    lua_rawseti(lua, -2, 3);
  } else {
    lua_file_new_(lua, pf->base);
    lua_rawseti(lua, -2, 1);
  }
  pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);
  lua_promise_finalize_(pro, true);
}

static void lua_promise_require_pathfind_cb_(upd_req_pathfind_t* pf) {
  promise_t_* pro = (void*) pf;
  upd_file_t* stf = pro->file;

  if (HEDLEY_UNLIKELY(pf->len)) {
    logf_(stf, "unknown path: %s", pf->udata);
    goto ABORT;
  }

  upd_file_t* f = pf->base;
  if (HEDLEY_UNLIKELY(f->driver != &upd_driver_lua)) {
    logf_(stf, "not lua script: %s", pf->udata);
    goto ABORT;
  }

  pro->req = (upd_req_t) {
    .file = f,
    .type = UPD_REQ_PROG_COMPILE,
    .cb   = lua_promise_require_compile_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&pro->req))) {
    goto ABORT;
  }
  return;

ABORT:
  lua_promise_finalize_(pro, false);
}

static void lua_promise_require_hook_abort_cb_(lua_State* lua, lua_Debug* dbg) {
  (void) dbg;
  luaL_error(lua, "module script reaches instruction limit");
}
static void lua_promise_require_hook_cb_(lua_State* lua, lua_Debug* dbg) {
  (void) dbg;
  lua_sethook(lua, lua_promise_require_hook_abort_cb_, LUA_MASKLINE, 0);
}
static void lua_promise_require_compile_cb_(upd_req_t* req) {
  promise_t_* pro = (void*) req;
  upd_file_t* stf = pro->file;
  stream_t_*  st  = stf->ctx;
  upd_file_t* prf = req->file;
  prog_t_*    pr  = prf->ctx;
  lua_State*  lua = st->lua;

  pro->registry.result = LUA_REFNIL;
  if (HEDLEY_UNLIKELY(!pr->compiled)) {
    goto EXIT;
  }

  lua_State* th = lua_newthread(lua);
  if (HEDLEY_UNLIKELY(th == NULL)) {
    logf_(prf, "thread allocation failure X(\n");
    goto EXIT;
  }
  lua_sethook(th,
    lua_promise_require_hook_cb_, LUA_MASKCOUNT, SANDBOX_INSTRUCTION_LIMIT_);

  lua_rawgeti(th, LUA_REGISTRYINDEX, pr->registry.func);
  lua_createtable(th, 0, 0);
  {
    lua_getfield(th, LUA_REGISTRYINDEX, "Lua");
    lua_setfield(th, -2, "Lua");
  }
  lua_setfenv(th, -2);
  const int result = lua_pcall(th, 0, LUA_MULTRET, 0);
  if (HEDLEY_UNLIKELY(result != 0)) {
    logf_(prf, "require failure: %s\n", lua_tostring(th, -1));
    goto EXIT;
  }

  const int n = lua_gettop(th);
  lua_createtable(lua, n, 0);
  const int t = lua_gettop(lua);

  lua_xmove(th, lua, n);
  for (int i = 1; i <= n; ++i) {
    lua_rawseti(lua, t, n-i+1);
  }
  pro->registry.result = luaL_ref(lua, LUA_REGISTRYINDEX);

EXIT:
  lua_promise_finalize_(pro, true);
}
