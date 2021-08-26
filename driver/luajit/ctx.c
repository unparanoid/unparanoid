#include "common.h"

#include "ctx_req.h"


static void lock_cb_(upd_file_lock_t* k) {
  lj_promise_t* pro = k->udata;
  upd_file_t*   stf = pro->stream;
  lj_stream_t*  st  = stf->ctx;
  lua_State*    L   = st->L;

  if (HEDLEY_LIKELY(k->ok)) {
    lj_lock_new(stf, k);
    pro->registry.result = luaL_ref(L, LUA_REGISTRYINDEX);
    lj_promise_finalize(pro, true);
  } else {
    upd_free(&k);
    lj_promise_finalize(pro, false);
  }
}
static int lock_(lua_State* L) {
  upd_file_t* stf = lua_touserdata(L, lua_upvalueindex(1));
  const bool  ex  = lua_toboolean(L, lua_upvalueindex(2));

  upd_file_t** udata = luaL_checkudata(L, 1, "std_File");
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return luaL_error(L, "file has been torn down");
  }
  upd_file_t* f = *udata;

  const lua_Integer timeout = lua_tointeger(L, 2);
  if (HEDLEY_UNLIKELY(timeout < 0)) {
    return luaL_error(L, "negative timeout");
  }

  lj_promise_t* pro   = lj_promise_new(stf);
  const int     index = lua_gettop(L);

  upd_file_lock_t* k = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&k, sizeof(*k)))) {
    lj_promise_finalize(pro, false);
    goto EXIT;
  }
  *k = (upd_file_lock_t) {
    .file    = f,
    .ex      = ex,
    .timeout = timeout,
    .udata   = pro,
    .cb      = lock_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(k))) {
    lj_promise_finalize(pro, false);
    goto EXIT;
  }

EXIT:
  lua_pushvalue(L, index);
  return 1;
}


static void pathfind_cb_(upd_pathfind_t* pf) {
  lj_promise_t* pro = pf->udata;
  upd_file_t*   stf = pro->stream;
  upd_iso_t*    iso = stf->iso;
  lj_stream_t*  st  = stf->ctx;
  lua_State*    L   = st->L;

  lua_createtable(L, 0, 0);
  if (HEDLEY_UNLIKELY(pf->len)) {
    lua_pushnil(L);
    lua_rawseti(L, -2, 1);

    lua_pushlstring(L, (char*) pf->path, pf->len);
    lua_rawseti(L, -2, 2);

    lj_file_new(stf, pf->base);
    lua_rawseti(L, -2, 3);
  } else {
    lj_file_new(stf, pf->base);
    lua_rawseti(L, -2, 1);
  }
  upd_iso_unstack(iso, pf);

  pro->registry.result = luaL_ref(L, LUA_REGISTRYINDEX);
  lj_promise_finalize(pro, true);
}
static int pathfind_(lua_State* L) {
  upd_file_t* stf = lua_touserdata(L, lua_upvalueindex(1));

  size_t len;
  const char* path = luaL_checklstring(L, 1, &len);

  lj_promise_t* pro   = lj_promise_new(stf);
  const int     index = lua_gettop(L);

  const bool ok = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso   = stf->iso,
      .path  = (uint8_t*) path,
      .len   = len,
      .udata = pro,
      .cb    = pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    lj_promise_finalize(pro, false);
  }
  lua_pushvalue(L, index);
  return 1;
}


static void require_hook_destruct_cb_(lua_State* L, lua_Debug* dbg) {
  (void) dbg;
  luaL_error(L, "module script reaches instruction limit");
}
static void require_hook_cb_(lua_State* L, lua_Debug* dbg) {
  (void) dbg;
  lua_sethook(L, require_hook_destruct_cb_, LUA_MASKLINE, 0);
}

static void require_compile_cb_(lj_compile_t* cp) {
  lj_promise_t* pro = cp->udata;
  upd_file_t*   stf = pro->stream;
  upd_iso_t*    iso = stf->iso;
  lj_stream_t*  st  = stf->ctx;

  const int func = cp->ok? cp->result: LUA_REFNIL;
  upd_iso_unstack(iso, cp);

  if (HEDLEY_UNLIKELY(func == LUA_REFNIL)) {
    lj_promise_finalize(pro, false);
    return;
  }

  lua_State* L = lua_newthread(st->L);
  if (HEDLEY_UNLIKELY(L == NULL)) {
    lj_promise_finalize(pro, false);
    return;
  }
  lua_sethook(L, require_hook_cb_, LUA_MASKCOUNT, LJ_INSTRUCTION_LIMIT);

  lua_rawgeti(L, LUA_REGISTRYINDEX, func);
  lua_createtable(L, 0, 0);
  {
    lua_getfield(L, LUA_REGISTRYINDEX, "std");
    lua_setfield(L, -2, "std");
  }
  lua_setfenv(L, -2);

  const int ret = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (HEDLEY_UNLIKELY(ret != LUA_OK)) {
    lj_promise_finalize(pro, false);
    return;
  }

  const int n = lua_gettop(L);
  lua_createtable(st->L, n, 0);
  const int t = lua_gettop(st->L);

  lua_xmove(L, st->L, n);
  for (int i = 1; i <= n; ++i) {
    lua_rawseti(st->L, t, n-i+1);
  }

  pro->registry.result = luaL_ref(st->L, LUA_REGISTRYINDEX);
  lj_promise_finalize(pro, true);
}
static void require_pathfind_cb_(upd_pathfind_t* pf) {
  lj_promise_t* pro = pf->udata;
  upd_file_t*   stf = pro->stream;
  upd_iso_t*    iso = stf->iso;

  upd_file_t* tar = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(tar == NULL)) {
    lj_promise_finalize(pro, false);
    return;
  }
  if (HEDLEY_UNLIKELY(tar->driver != &lj_prog)) {
    lj_promise_finalize(pro, false);
    return;
  }

  const bool ok = lj_compile_with_dup(&(lj_compile_t) {
      .prog  = tar,
      .udata = pro,
      .cb    = require_compile_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    lj_promise_finalize(pro, false);
    return;
  }
}
static int require_(lua_State* L) {
  upd_file_t* stf = lua_touserdata(L, lua_upvalueindex(1));

  size_t len;
  const char* path = luaL_checklstring(L, 1, &len);

  lj_promise_t* pro   = lj_promise_new(stf);
  const int     index = lua_gettop(L);

  const bool ok = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso   = stf->iso,
      .path  = (uint8_t*) path,
      .len   = len,
      .udata = pro,
      .cb    = require_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    lj_promise_finalize(pro, false);
  }
  lua_pushvalue(L, index);
  return 1;
}


static int sleep_(lua_State* L) {
  upd_file_t*  f   = lua_touserdata(L, lua_upvalueindex(1));
  lj_stream_t* ctx = f->ctx;

  ctx->state = LJ_STREAM_WAITING_TIMER;

  const lua_Integer t = luaL_checkinteger(L, 1);
  if (HEDLEY_UNLIKELY(t < 0)) {
    return luaL_error(L, "negative duration: %"PRIiMAX"", (intmax_t) t);
  }

  if (HEDLEY_UNLIKELY(!upd_file_trigger_timer(f, t))) {
    return luaL_error(L, "failed to set timer for lua runner");
  }
  return lua_yield(L, 0);
}


static int recv_push_cb_(lj_watcher_t* w) {
  upd_file_t*  stf = w->udata;
  lj_stream_t* ctx = stf->ctx;
  lua_State*   L   = ctx->L;

  if (HEDLEY_LIKELY(!ctx->in.size)) {
    return 0;
  }
  lua_pushlstring(L, (char*) ctx->in.ptr, ctx->in.size);
  upd_buf_clear(&ctx->in);
  return 1;
}
static void recv_unwatch_cb_(lj_watcher_t* w) {
  upd_file_t*  stf = w->udata;
  lj_stream_t* ctx = stf->ctx;

  ctx->recv = NULL;
}
static int recv_(lua_State* L) {
  upd_file_t*  stf = lua_touserdata(L, lua_upvalueindex(1));
  lj_stream_t* ctx = stf->ctx;

  if (HEDLEY_UNLIKELY(ctx->recv)) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->recv->registry.self);
    return 1;
  }

  lj_watcher_t* w     = lj_watcher_new(stf, 0);
  const int     index = lua_gettop(L);

  w->udata   = stf;
  w->push    = recv_push_cb_;
  w->unwatch = recv_unwatch_cb_;

  ctx->recv = w;
  if (HEDLEY_UNLIKELY(ctx->in.size)) {
    lj_watcher_trigger(w);
  }
  lua_pushvalue(L, index);
  return 1;
}


static int send_(lua_State* L) {
  upd_file_t*  f   = lua_touserdata(L, lua_upvalueindex(1));
  lj_stream_t* ctx = f->ctx;

  const size_t n = lua_gettop(L);
  for (size_t i = 1; i <= n; ++i) {
    size_t len;
    const char* s = luaL_checklstring(L, i, &len);

    if (HEDLEY_UNLIKELY(len && !upd_buf_append(&ctx->out, (uint8_t*) s, len))) {
      return luaL_error(L, "buffer allocation failure");
    }
  }
  return 0;
}


typedef struct {
  lj_watcher_t super;

  upd_file_watch_t watch;

  struct {
    upd_file_event_t ptr[32];
    size_t           n;
  } events;
} watch_t_;

static void watch_cb_(upd_file_watch_t* watch) {
  watch_t_* w = watch->udata;
  if (HEDLEY_UNLIKELY(w->events.n >= sizeof(w->events.ptr)/sizeof(w->events.ptr[0]))) {
    return;
  }
  switch (watch->event) {
  case UPD_FILE_UPDATE:
  case UPD_FILE_DELETE:
    w->events.ptr[w->events.n++] = watch->event;
    lj_watcher_trigger(&w->super);
    break;
  default:
    break;
  }
}
static int watch_push_cb_(lj_watcher_t* super) {
  watch_t_*    w   = super->udata;
  upd_file_t*  stf = w->super.stream;
  lj_stream_t* st  = stf->ctx;
  lua_State*   L   = st->L;

  if (HEDLEY_UNLIKELY(w->events.n == 0)) {
    return 0;
  }

  switch (w->events.ptr[0]) {
  case UPD_FILE_UPDATE:
    lua_pushstring(L, "UPDATE");
    break;
  case UPD_FILE_DELETE:
    lua_pushstring(L, "DELETE");
    break;
  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }

  --w->events.n;
  memmove(w->events.ptr, w->events.ptr+1, sizeof(w->events.ptr[0])*w->events.n);
  return 1;
}
static void watch_unwatch_cb_(lj_watcher_t* super) {
  watch_t_* w = super->udata;
  upd_file_unwatch(&w->watch);
}
static int watch_(lua_State* L) {
  upd_file_t* stf = lua_touserdata(L, lua_upvalueindex(1));

  upd_file_t* f = *(void**) luaL_checkudata(L, 1, "std_File");
  if (HEDLEY_UNLIKELY(f == NULL)) {
    return luaL_error(L, "invalid file");
  }

  static const size_t addsize = sizeof(watch_t_) - sizeof(lj_watcher_t);

  watch_t_* w     = (void*) lj_watcher_new(stf, addsize);
  const int index = lua_gettop(L);

  w->super.udata   = w;
  w->super.push    = watch_push_cb_;
  w->super.unwatch = watch_unwatch_cb_;

  w->events.n = 0;

  w->watch = (upd_file_watch_t) {
    .file  = f,
    .udata = w,
    .cb    = watch_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&w->watch))) {
    return luaL_error(L, "watcher insertion failure");
  }
  lua_pushvalue(L, index);
  return 1;
}


void lj_ctx_create(lua_State* L, upd_file_t* stf) {
  *(void**) lua_newuserdata(L, sizeof(stf)) = stf;
  {
    lua_createtable(L, 0, 0);
    {
      lua_createtable(L, 0, 0);
      {
        lj_file_new(stf, stf);
        lua_setfield(L, -2, "stream");

        lua_pushlightuserdata(L, stf);
        lua_pushboolean(L, false);
        lua_pushcclosure(L, lock_, 2);
        lua_setfield(L, -2, "lock");

        lua_pushlightuserdata(L, stf);
        lua_pushboolean(L, true);
        lua_pushcclosure(L, lock_, 2);
        lua_setfield(L, -2, "lockEx");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, pathfind_, 1);
        lua_setfield(L, -2, "pathfind");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, recv_, 1);
        lua_setfield(L, -2, "recv");

        lua_pushlightuserdata(L, stf);
        req_create_(L);
        lua_setfield(L, -2, "req");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, require_, 1);
        lua_setfield(L, -2, "require");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, send_, 1);
        lua_setfield(L, -2, "send");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, sleep_, 1);
        lua_setfield(L, -2, "sleep");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, watch_, 1);
        lua_setfield(L, -2, "watch");
      }
      lua_setfield(L, -2, "__index");

      lua_pushcfunction(L, lj_lua_immutable_newindex);
      lua_setfield(L, -2, "__newindex");
    }
    lua_setmetatable(L, -2);
  }
}
