#include "common.h"


#define LOG_PREFIX_ "upd.luajit.stream: "


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

static
void
stream_exit_(
  upd_file_t* f,
  bool        ok);

static
void
stream_teardown_(
  upd_file_t* f);

static
void
stream_create_env_(
  upd_file_t* f);

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
stream_logf_(
  upd_file_t* f,
  const char* fmt,
  ...);

const upd_driver_t lj_stream = {
  .name = (uint8_t*) "upd.luajit.stream_",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_DSTREAM,
    0,
  },
  .flags = {
    .timer = true,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


static
void
stream_watch_cb_(
  upd_file_watch_t* w);

static
void
stream_lua_hook_cb_(
  lua_State* L,
  lua_Debug* dbg);


bool lj_stream_start(upd_file_t* f) {
  lj_stream_t* ctx    = f->ctx;
  lj_dev_t*    devctx = ctx->dev->ctx;
  lua_State*   L      = devctx->L;

  L = lua_newthread(L);
  if (HEDLEY_UNLIKELY(L == NULL)) {
    stream_logf_(f, "failed to create new thread");
    return false;
  }
  ctx->L = L;

  if (HEDLEY_UNLIKELY(!upd_file_trigger_timer(f, 0))) {
    stream_logf_(f, "failed to start runner");
    return false;
  }

  lua_pushthread(L);
  ctx->registry.thread = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_sethook(L, stream_lua_hook_cb_, LUA_MASKCOUNT, LJ_INSTRUCTION_LIMIT);

  stream_create_env_(f);
  ctx->registry.ctx = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_pushthread(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->registry.ctx);
  lua_setfenv(L, -2);
  lua_pop(L, 1);

  lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->registry.func);
  ctx->registry.func = luaL_ref(L, LUA_REGISTRYINDEX);
  return true;
}

bool lj_stream_resume(upd_file_t* f) {
  if (HEDLEY_UNLIKELY(!upd_file_trigger_timer(f, 0))) {
    stream_logf_(f, "failed to start lua runner");
    stream_exit_(f, false);
    return false;
  }
  return true;
}

upd_file_t* lj_stream_get(lua_State* L) {
  lua_pushthread(L);
  lua_getfenv(L, -1);
  lua_getfield(L, -1, "ctx");
  upd_file_t** f = lua_touserdata(L, -1);
  lua_pop(L, 3);
  return f? *f: NULL;
}


static bool stream_init_(upd_file_t* f) {
  lj_stream_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (lj_stream_t) {
    .state = LJ_STREAM_RUNNING,
    .watch = {
      .file  = f,
      .udata = f,
      .cb    = stream_watch_cb_,
    },
  };

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    return false;
  }
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  lj_stream_t* ctx = f->ctx;
  lua_State*   L   = ctx->L;

  upd_file_unwatch(&ctx->watch);

  stream_teardown_(f);

  upd_buf_clear(&ctx->in);
  upd_buf_clear(&ctx->out);

  if (HEDLEY_LIKELY(L)) {
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->registry.thread);
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->registry.func);
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->registry.ctx);
  }

  upd_file_unref(ctx->dev);
  upd_file_unref(ctx->prog);

  upd_free(&ctx);
}

static bool stream_handle_(upd_req_t* req) {
  upd_file_t*  f   = req->file;
  lj_stream_t* ctx = f->ctx;

  switch (req->type) {
  case UPD_REQ_DSTREAM_READ: {
    const bool alive = ctx->state & LJ_STREAM_RUNNING;
    if (HEDLEY_UNLIKELY(ctx->out.size == 0 && !alive)) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    req->stream.io = (upd_req_stream_io_t) {
      .buf  = ctx->out.ptr,
      .size = ctx->out.size,
      .tail = !alive,
    };

    upd_buf_t temp = ctx->out;
    ctx->out = (upd_buf_t) {0};

    req->result = UPD_REQ_OK;
    req->cb(req);

    upd_buf_clear(&temp);
  } return true;

  case UPD_REQ_DSTREAM_WRITE: {
    const upd_req_stream_io_t* io = &req->stream.io;
    if (HEDLEY_UNLIKELY(!(ctx->state & LJ_STREAM_RUNNING))) {
      req->result = UPD_REQ_ABORTED;
      return false;
    }
    if (HEDLEY_UNLIKELY(!upd_buf_append(&ctx->in, io->buf, io->size))) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    if (HEDLEY_UNLIKELY(ctx->state == LJ_STREAM_PENDING_INPUT)) {
      if (HEDLEY_UNLIKELY(!lj_stream_resume(f))) {
        req->result = UPD_REQ_ABORTED;
        return false;
      }
    }
    req->cb(req);
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static void stream_exit_(upd_file_t* f, bool ok) {
  lj_stream_t* ctx = f->ctx;

  ctx->state = ok? LJ_STREAM_EXITED: LJ_STREAM_ABORTED;

  stream_teardown_(f);
  upd_file_trigger(f, UPD_FILE_UPDATE);
}

static void stream_teardown_(upd_file_t* f) {
  lj_stream_t* ctx = f->ctx;

  for (size_t i = 0; i < ctx->files.n; ++i) {
    upd_file_t** udata = ctx->files.p[i];
    if (HEDLEY_LIKELY(*udata && *udata != f)) {
      upd_file_unref(*udata);
    }
    *udata = NULL;
  }
  upd_array_clear(&ctx->files);

  for (size_t i = 0; i < ctx->locks.n; ++i) {
    upd_file_lock_t** udata = ctx->locks.p[i];
    if (HEDLEY_LIKELY(*udata)) upd_file_unlock(*udata);
    upd_free(&*udata);
  }
  upd_array_clear(&ctx->locks);
}

static void stream_logf_(upd_file_t* f, const char* fmt, ...) {
  upd_iso_t* iso = f->iso;

  upd_iso_msgf(iso, LOG_PREFIX_);

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(iso, fmt, args);
  va_end(args);

  upd_iso_msgf(iso, " (%s)\n", f->npath);
}

static void stream_create_env_(upd_file_t* f) {
  lj_stream_t* ctx = f->ctx;
  lua_State*   L   = ctx->L;

  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 2);
    {
      lua_createtable(L, 0, 4);
      {
        lj_ctx_create(L, f);
        lua_setfield(L, -2, "ctx");

        lua_getfield(L, LUA_REGISTRYINDEX, "std");
        lua_setfield(L, -2, "std");
      }
      lua_setfield(L, -2, "__index");

      lua_pushcfunction(L, lj_lua_immutable_newindex);
      lua_setfield(L, -2, "__newindex");
    }
    lua_setmetatable(L, -2);
  }
}


static void stream_watch_cb_(upd_file_watch_t* w) {
  upd_file_t*  f   = w->udata;
  lj_stream_t* ctx = f->ctx;
  lua_State*   L   = ctx->L;

  if (HEDLEY_UNLIKELY(L == NULL || w->event != UPD_FILE_TIMER)) {
    return;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->registry.func);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->registry.ctx);
  lua_setfenv(L, -2);

  int args = 0;
  switch (ctx->state) {
  case LJ_STREAM_PENDING_INPUT:
    args = 1;
    lua_pushlstring(L, (char*) ctx->in.ptr, ctx->in.size);
    upd_buf_clear(&ctx->in);
    break;
  case LJ_STREAM_PENDING_PROMISE:
    args = lj_promise_push_result(ctx->pending);
    break;
  default:
    break;
  }
  ctx->state = LJ_STREAM_RUNNING;

  const int err = lua_resume(L, args);
  switch (err) {
  case 0:
    stream_exit_(f, true);
    break;

  case LUA_YIELD:
    break;

  case LUA_ERRRUN:
  case LUA_ERRMEM:
  case LUA_ERRERR: {
    lua_Debug dbg;
    lua_getstack(L, 0, &dbg);
    lua_getinfo(L, "Sl", &dbg);
    stream_logf_(f, "runtime error: %s (%s:%d)",
      lua_tostring(L, -1), dbg.source, dbg.currentline);
    stream_exit_(f, false);
  } break;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }
  lua_settop(L, 0);

  /*  upd_file_trigger could destroy the file unexpectedly
   * without this reference... */
  upd_file_ref(f);
  {
    bool update = !!ctx->out.size;
    switch (ctx->state) {
    case LJ_STREAM_RUNNING:
      lj_stream_resume(f);
      break;

    case LJ_STREAM_EXITED:
    case LJ_STREAM_ABORTED:
      update = true;
      break;

    default:
      break;
    }
    if (HEDLEY_UNLIKELY(update)) {
      upd_file_trigger(f, UPD_FILE_UPDATE);
    }
  }
  upd_file_unref(f);
}

static void stream_lua_destruct_hook_cb_(lua_State* L, lua_Debug* dbg) {
  (void) dbg;
  luaL_error(L, "reaches instruction limit %d", LJ_INSTRUCTION_LIMIT);
}
static void stream_lua_hook_cb_(lua_State* L, lua_Debug* dbg) {
  (void) dbg;
  lua_sethook(L, stream_lua_destruct_hook_cb_, LUA_MASKLINE, 0);
}
