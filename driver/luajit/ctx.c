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

  ctx->state = LJ_STREAM_PENDING_TIMER;

  const lua_Integer t = luaL_checkinteger(L, 1);
  if (HEDLEY_UNLIKELY(t < 0)) {
    return luaL_error(L, "negative duration: %"PRIiMAX"", (intmax_t) t);
  }

  if (HEDLEY_UNLIKELY(!upd_file_trigger_timer(f, t))) {
    return luaL_error(L, "failed to set timer for lua runner");
  }
  return lua_yield(L, 0);
}

static int recv_(lua_State* L) {
  upd_file_t*  f   = lua_touserdata(L, lua_upvalueindex(1));
  lj_stream_t* ctx = f->ctx;

  lua_pushlstring(L, (char*) ctx->in.ptr, ctx->in.size);
  upd_buf_clear(&ctx->in);
  return 1;
}

static int recv_blocked_(lua_State* L) {
  upd_file_t*  f   = lua_touserdata(L, lua_upvalueindex(1));
  lj_stream_t* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->in.size == 0)) {
    ctx->state = LJ_STREAM_PENDING_INPUT;
    return lua_yield(L, 0);
  }

  lua_pushlstring(L, (char*) ctx->in.ptr, ctx->in.size);
  upd_buf_clear(&ctx->in);
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
        lua_pushcclosure(L, lock_, 1);
        lua_setfield(L, -2, "lock");

        lua_pushlightuserdata(L, stf);
        lua_pushboolean(L, true);
        lua_pushcclosure(L, lock_, 1);
        lua_setfield(L, -2, "lockEx");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, pathfind_, 1);
        lua_setfield(L, -2, "pathfind");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, recv_, 1);
        lua_setfield(L, -2, "recv");

        lua_pushlightuserdata(L, stf);
        lua_pushcclosure(L, recv_blocked_, 1);
        lua_setfield(L, -2, "recvBlocked");

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
      }
      lua_setfield(L, -2, "__index");

      lua_pushcfunction(L, lj_lua_immutable_newindex);
      lua_setfield(L, -2, "__newindex");
    }
    lua_setmetatable(L, -2);
  }
}
