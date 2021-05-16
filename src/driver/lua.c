#include "common.h"


#define SCRIPT_MAX_ (1024*1024*8)

#define DEV_PATH_ "/sys/upd.dev.lua"

#define SANDBOX_INSTRUCTION_LIMIT_ 10000000  /* = 10^7 (10ms in 1 GHz clock) */


static const char* lua_global_[] = {
  "iso",
};


typedef struct prog_t_ {
  upd_file_t* file;
  upd_file_t* dev;

  upd_file_watch_t watch;

  int func_registry_index;

  size_t   size;
  uint8_t* buf;
  uv_file  fd;

  unsigned buf_on_stack : 1;
  unsigned clean        : 1;
  unsigned compiled     : 1;
} prog_t_;

typedef struct stream_t_ {
  uv_idle_t idle;

  lua_State* lua;

  upd_buf_t in;
  upd_buf_t out;

  /* strong reference */
  upd_file_t* file;
  upd_file_t* dev;
  upd_file_t* prog;
  int         lua_registry_index;
  int         env_registry_index;
  int         func_registry_index;

  unsigned dirty   : 1;
  unsigned holdref : 1;
  unsigned exited  : 1;
} stream_t_;


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

const upd_driver_t upd_driver_lua = {
  .name = (uint8_t*) "upd.lua",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_BIN,
    UPD_REQ_PROG,
    0,
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
lua_logf_(
  upd_file_t* f,
  const char* fmt,
  ...);


static
void
stream_exit_(
  upd_file_t* st);

static
void
stream_resume_(
  upd_file_t* st);


static
void
stream_lua_build_class_(
  lua_State* lua);

static
void
stream_lua_new_(
  lua_State*  lua,
  upd_file_t* f);


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
prog_lock_for_exec_cb_(
  upd_file_lock_t* lock);

static
void
prog_lock_for_compile_cb_(
  upd_file_lock_t* lock);

static
void
prog_exec_after_compile_cb_(
  upd_req_t* req);

static
void
prog_lock_for_delete_cb_(
  upd_file_lock_t* lock);


static
void
stream_lua_hook_cb_(
  lua_State* state,
  lua_Debug* dbg);

static
void
stream_idle_cb_(
  uv_idle_t* idle);

static
void
stream_close_cb_(
  uv_handle_t* handle);


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

  upd_file_unwatch(&ctx->watch);

  bool lock = dev != NULL && ctx->compiled;
  if (HEDLEY_LIKELY(lock)) {
    lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = dev,
        .ex    = true,
        .udata = ctx,
        .cb    = prog_lock_for_delete_cb_,
      });
  }

  if (HEDLEY_LIKELY(dev)) {
    upd_file_unref(dev);
  }
  if (HEDLEY_LIKELY(lock)) {
    return;
  }
  upd_free(&ctx);
}

static bool prog_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  prog_t_*    ctx = f->ctx;
  upd_iso_t*  iso = f->iso;
  upd_file_t* dev = ctx->dev;

  switch (req->type) {
  case UPD_REQ_BIN_ACCESS:
    req->bin.access = (upd_req_bin_access_t) {
      .write = !f->npath,
    };
    break;
  case UPD_REQ_BIN_WRITE: {
    if (HEDLEY_UNLIKELY(f->npath || req->bin.rw.offset)) {
      return false;
    }
    ctx->buf_on_stack = false;
    ctx->buf  = req->bin.rw.buf;
    ctx->size = req->bin.rw.size;
    upd_file_ref(f);
    const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = dev,
        .ex    = true,
        .udata = req,
        .cb    = prog_lock_for_compile_cb_,
      });
    if (HEDLEY_UNLIKELY(!lock)) {
      upd_file_unref(f);
      return false;
    }
  } break;

  case UPD_REQ_PROG_ACCESS:
    req->prog.access = (upd_req_prog_access_t) {
      .exec = true,
    };
    break;

  case UPD_REQ_PROG_EXEC: {
    req->prog.exec = NULL;
    if (HEDLEY_LIKELY(ctx->clean && ctx->compiled)) {
      upd_file_ref(f);
      const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
          .file  = dev,
          .ex    = true,
          .udata = req,
          .cb    = prog_lock_for_exec_cb_,
        });
      if (HEDLEY_UNLIKELY(!lock)) {
        upd_file_unref(f);
        return false;
      }
      return true;
    }
    if (HEDLEY_UNLIKELY(!f->npath)) {
      return false;
    }

    upd_req_t* wrap = upd_iso_stack(iso, sizeof(*wrap));
    if (HEDLEY_UNLIKELY(wrap == NULL)) {
      return false;
    }
    *wrap = (upd_req_t) {
      .file  = f,
      .udata = req,
      .cb    = prog_exec_after_compile_cb_,
    };

    uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
    if (HEDLEY_UNLIKELY(fsreq == NULL)) {
      return false;
    }
    *fsreq = (uv_fs_t) { .data = wrap, };

    upd_file_ref(f);
    const bool stat =
      0 <= uv_fs_stat(&iso->loop, fsreq, (char*) f->npath, prog_stat_cb_);
    if (HEDLEY_UNLIKELY(!stat)) {
      upd_file_unref(f);
      return false;
    }
  } return true;

  default:
    return false;
  }
  req->cb(req);
  return true;
}


static bool stream_init_(upd_file_t* f) {
  stream_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (stream_t_) {
    .file = f,
  };

  if (HEDLEY_UNLIKELY(0 > uv_idle_init(&f->iso->loop, &ctx->idle))) {
    upd_free(&ctx);
    return false;
  }
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->dev)) {
    upd_file_unref(ctx->dev);
  }
  if (HEDLEY_LIKELY(ctx->prog)) {
    upd_file_unref(ctx->prog);
  }
  if (HEDLEY_LIKELY(ctx->lua)) {
    luaL_unref(ctx->lua, LUA_REGISTRYINDEX, ctx->env_registry_index);
    luaL_unref(ctx->lua, LUA_REGISTRYINDEX, ctx->func_registry_index);
    luaL_unref(ctx->lua, LUA_REGISTRYINDEX, ctx->lua_registry_index);
  }
  uv_close((uv_handle_t*) &ctx->idle, stream_close_cb_);
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
    if (HEDLEY_UNLIKELY(ctx->exited)) {
      return false;
    }
    const upd_req_stream_io_t io = req->stream.io;
    req->stream.io.size = 0;
    if (HEDLEY_UNLIKELY(!upd_buf_append(&ctx->in, io.buf, io.size))) {
      return false;
    }
    ctx->dirty = true;
    req->stream.io.size = io.size;
  } break;

  case UPD_REQ_STREAM_OUTPUT:
    if (HEDLEY_UNLIKELY(ctx->out.size == 0 && ctx->exited)) {
      return false;
    }
    req->stream.io = (upd_req_stream_io_t) {
      .buf  = ctx->out.ptr,
      .size = ctx->out.size,
    };
    req->cb(req);
    upd_buf_clear(&ctx->out);
    return true;

  default:
    return false;
  }
  req->cb(req);
  return true;
}


static void lua_logf_(upd_file_t* f, const char* fmt, ...) {
  upd_iso_t* iso = f->iso;

  upd_iso_msgf(iso, "upd.lua error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(iso, fmt, args);
  va_end(args);

  upd_iso_msgf(iso, " (%s)\n", f->npath);
}


static void stream_exit_(upd_file_t* st) {
  stream_t_* ctx = st->ctx;

  upd_file_trigger(st, UPD_FILE_UPDATE);

  ctx->exited = true;
  if (HEDLEY_LIKELY(ctx->holdref)) {
    upd_file_unref(st);
  }
  upd_file_trigger(st, UPD_FILE_UPDATE);
}

static void stream_resume_(upd_file_t* st) {
  stream_t_* ctx = st->ctx;
  lua_State* lua = ctx->lua;

  lua_rawgeti(lua, LUA_REGISTRYINDEX, ctx->func_registry_index);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, ctx->env_registry_index);
  lua_setfenv(lua, -2);
  lua_pop(lua, 1);

  const int err = lua_resume(lua, 0);
  switch (err) {
  case 0:
    lua_settop(lua, 0);
    stream_exit_(st);
    break;
  case LUA_YIELD:
    lua_settop(lua, 0);
    if (HEDLEY_UNLIKELY(!uv_is_active((uv_handle_t*) &ctx->idle))) {
      if (HEDLEY_UNLIKELY(0 > uv_idle_start(&ctx->idle, stream_idle_cb_))) {
        lua_logf_(st, "failed to start runner");
        stream_exit_(st);
      }
    }
    break;
  case LUA_ERRRUN:
  case LUA_ERRMEM:
  case LUA_ERRERR: {
    lua_Debug dbg;
    lua_getstack(lua, 0, &dbg);
    lua_getinfo(lua, "Sl", &dbg);
    lua_logf_(st,
      "runtime error (%s) (%s:%d)",
      lua_tostring(lua, -1), dbg.source, dbg.currentline);
    stream_exit_(st);
  } break;
  }
}


static int stream_lua_output_(lua_State* lua) {
  upd_file_t* f   = *(void**) lua_touserdata(lua, lua_upvalueindex(1));
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 1)) {
    return luaL_error(lua, "ProgramStream.output() takes just one arguments");
  }

  size_t len;
  const char* ptr = luaL_checklstring(lua, 1, &len);

  if (HEDLEY_UNLIKELY(!upd_buf_append(&ctx->out, (uint8_t*) ptr, len))) {
    return luaL_error(lua, "buffer allocation failure");
  }
  upd_file_trigger(f, UPD_FILE_UPDATE);
  return 0;
}
static int stream_lua_yield_(lua_State* lua) {
  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 0)) {
    return luaL_error(lua, "ProgramStream.yield() takes no arguments");
  }
  return lua_yield(lua, 0);
}
static int stream_lua_unref_(lua_State* lua) {
  upd_file_t* f   = *(void**) lua_touserdata(lua, lua_upvalueindex(1));
  stream_t_*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(lua_gettop(lua) != 0)) {
    return luaL_error(lua, "ProgramStream.unref() takes no arguments");
  }

  lua_pushboolean(lua, ctx->holdref);

  if (HEDLEY_LIKELY(ctx->holdref)) {
    upd_file_unref(f);
    ctx->holdref = false;
  }
  return 1;
}
static int stream_lua_index_(lua_State* lua) {
  upd_file_t* f   = *(void**) lua_touserdata(lua, 1);
  stream_t_*  ctx = f->ctx;

  const char* key = luaL_checkstring(lua, 2);
  if (HEDLEY_UNLIKELY(utf8cmp(key, "input") == 0)) {
    lua_pushlstring(lua, (char*) ctx->in.ptr, ctx->in.size);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "output") == 0)) {
    lua_pushvalue(lua, 1);
    lua_pushcclosure(lua, stream_lua_output_, 1);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "yield") == 0)) {
    lua_pushvalue(lua, 1);
    lua_pushcclosure(lua, stream_lua_yield_, 1);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "unref") == 0)) {
    lua_pushvalue(lua, 1);
    lua_pushcclosure(lua, stream_lua_unref_, 1);
    return 1;
  }
  return luaL_error(lua, "unknown field ProgramStream.%s", key);
}
static void stream_lua_build_class_(lua_State* lua) {
  luaL_newmetatable(lua, "ProgramStream");

  lua_pushcfunction(lua, stream_lua_index_);
  lua_setfield(lua, -2, "__index");

  lua_getfield(lua, LUA_REGISTRYINDEX, "File");
  lua_setfield(lua, -2, "__metatable");

  lua_pop(lua, 1);
}

static void stream_lua_new_(lua_State* lua, upd_file_t* f) {
  upd_file_t** ptr = lua_newuserdata(lua, sizeof(f));
  *ptr = f;

  lua_getfield(lua, LUA_REGISTRYINDEX, "ProgramStream");
  lua_setmetatable(lua, -2);
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
    lua_logf_(f, "'"DEV_PATH_"' is not found");
    goto EXIT;
  }

  if (HEDLEY_UNLIKELY(dev->driver != &upd_driver_dev_lua)) {
    lua_logf_(f, "'"DEV_PATH_"' is not lua_State device");
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
    lua_logf_(f, "stat failure");
    goto ABORT;
  }

  ctx->size = fsreq->statbuf.st_size;
  if (HEDLEY_UNLIKELY(ctx->size > SCRIPT_MAX_)) {
    lua_logf_(f, "too huge script (%zu exceeds %zu)", ctx->size, SCRIPT_MAX_);
    goto ABORT;
  }

  const bool open = 0 <= uv_fs_open(
    &iso->loop, fsreq, (char*) f->npath, 0, O_RDONLY, prog_open_cb_);
  if (HEDLEY_UNLIKELY(!open)) {
    lua_logf_(f, "open failure");
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
    lua_logf_(f, "open failure");
    goto ABORT;
  }
  ctx->fd = result;

  ctx->buf = upd_iso_stack(iso, ctx->size);
  if (HEDLEY_UNLIKELY(ctx->buf == NULL)) {
    lua_logf_(f, "read buffer allocation failure");
    goto ABORT;
  }
  ctx->buf_on_stack = true;

  uv_buf_t buf = uv_buf_init((char*) ctx->buf, ctx->size);

  const bool read =
    0 <= uv_fs_read(&iso->loop, fsreq, ctx->fd, &buf, 1, 0, prog_read_cb_);
  if (HEDLEY_UNLIKELY(!read)) {
    lua_logf_(f, "read failure");
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
    upd_iso_unstack(iso, ctx->buf);
    lua_logf_(f, "read failure");
    goto EXIT;
  }

  upd_file_ref(f);
  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = ctx->dev,
      .ex    = true,
      .udata = req,
      .cb    = prog_lock_for_compile_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_iso_unstack(iso, ctx->buf);
    req->cb(req);
    lua_logf_(f, "lock allocation failure");
    upd_file_unref(f);
    goto EXIT;
  }

EXIT:
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

static void prog_lock_for_compile_cb_(upd_file_lock_t* lock) {
  upd_req_t*  req = lock->udata;
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;
  prog_t_*    ctx = f->ctx;
  upd_file_t* dev = ctx->dev;
  lua_State*  lua = dev->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto EXIT;
  }

  lua_getfield(lua, LUA_REGISTRYINDEX, "ProgramStream");
  if (HEDLEY_UNLIKELY(lua_isnil(lua, -1))) {
    stream_lua_build_class_(lua);
  } else {
    lua_pop(lua, 1);
  }

  if (HEDLEY_LIKELY(ctx->compiled)) {
    luaL_unref(lua, LUA_REGISTRYINDEX, ctx->func_registry_index);
    ctx->compiled = false;
  }

  const int load = luaL_loadbuffer(
    lua, (char*) ctx->buf, ctx->size, (char*) f->npath);

  switch (load) {
  case LUA_ERRSYNTAX:
    lua_logf_(f, "lua parser syntax error");
    goto EXIT;
  case LUA_ERRMEM:
    lua_logf_(f, "lua parser allocation failure");
    goto EXIT;
  }

  ctx->compiled = true;
  ctx->clean    = true;
  ctx->func_registry_index = luaL_ref(lua, LUA_REGISTRYINDEX);

EXIT:
  req->cb(req);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  if (HEDLEY_LIKELY(ctx->buf_on_stack)) {
    upd_iso_unstack(iso, ctx->buf);
  }

  upd_file_unref(f);
}

static void prog_exec_after_compile_cb_(upd_req_t* req) {
  upd_file_t* f      = req->file;
  prog_t_*    ctx    = f->ctx;
  upd_iso_t*  iso    = f->iso;
  upd_req_t*  origin = req->udata;

  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(ctx->compiled)) {
    upd_file_ref(f);
    const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = ctx->dev,
        .ex    = true,
        .udata = origin,
        .cb    = prog_lock_for_exec_cb_,
      });
    if (HEDLEY_UNLIKELY(!lock)) {
      origin->cb(origin);
      upd_file_unref(f);
      return;
    }
  }
}

static void prog_lock_for_exec_cb_(upd_file_lock_t* lock) {
  upd_req_t*  req = lock->udata;
  upd_file_t* prf = req->file;
  upd_iso_t*  iso = prf->iso;
  prog_t_*    ctx = prf->ctx;
  upd_file_t* dev = ctx->dev;
  lua_State*  lua = dev->ctx;

  upd_file_t* stf = upd_file_new(iso, &stream_);
  if (HEDLEY_UNLIKELY(prf == NULL)) {
    goto EXIT;
  }

  stream_t_* st = stf->ctx;
  st->file = stf;
  st->dev  = dev;
  st->prog = prf;

  upd_file_ref(st->dev);
  upd_file_ref(st->prog);

  lua_State* th = lua_newthread(lua);
  if (HEDLEY_UNLIKELY(th == NULL)) {
    upd_file_unref(stf);
    goto EXIT;
  }

  st->lua = th;
  st->lua_registry_index = luaL_ref(lua, LUA_REGISTRYINDEX);

  lua_sethook(th,
    stream_lua_hook_cb_, LUA_MASKCOUNT, SANDBOX_INSTRUCTION_LIMIT_);

  lua_createtable(th, 0, 0);
  for (size_t i = 0; i < sizeof(lua_global_)/sizeof(lua_global_[0]); ++i) {
    lua_getglobal(th, lua_global_[i]);
    lua_setfield(th, -2, lua_global_[i]);
  }
  stream_lua_new_(th, stf);
  lua_setfield(th, -2, "stream");

  st->env_registry_index = luaL_ref(th, LUA_REGISTRYINDEX);

  lua_rawgeti(th, LUA_REGISTRYINDEX, ctx->func_registry_index);
  lua_pushvalue(th, -1);
  st->func_registry_index = luaL_ref(th, LUA_REGISTRYINDEX);

  st->holdref = true;
  upd_file_ref(stf);

  stream_resume_(stf);
  req->prog.exec = stf;

EXIT:
  req->cb(req);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  upd_file_unref(prf);
}

static void prog_lock_for_delete_cb_(upd_file_lock_t* lock) {
  prog_t_* ctx = lock->udata;
  upd_file_t*  dev = lock->file;
  upd_iso_t*   iso = dev->iso;
  lua_State*   lua = dev->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto EXIT;
  }

  if (HEDLEY_LIKELY(ctx->compiled)) {
    luaL_unref(lua, LUA_REGISTRYINDEX, ctx->func_registry_index);
  }

EXIT:
  upd_free(&ctx);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
}


static void stream_lua_destroy_hook_cb_(lua_State* lua, lua_Debug* dbg) {
  (void) dbg;
  luaL_error(lua, "reaches instruction limit %d", SANDBOX_INSTRUCTION_LIMIT_);
}
static void stream_lua_hook_cb_(lua_State* lua, lua_Debug* dbg) {
  (void) dbg;
  lua_sethook(lua, stream_lua_destroy_hook_cb_, LUA_MASKLINE, 0);
}

static void stream_idle_cb_(uv_idle_t* idle) {
  stream_t_*  ctx = (void*) idle;
  upd_file_t* f   = ctx->file;

  stream_resume_(f);

  if (HEDLEY_UNLIKELY(ctx->exited)) {
    uv_idle_stop(&ctx->idle);
  }
}

static void stream_close_cb_(uv_handle_t* handle) {
  stream_t_* ctx = (void*) handle;
  upd_free(&ctx);
}
