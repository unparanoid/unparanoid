#include "common.h"

#include "std_lua.h"
#include "std_mpk.h"
#include "std_path.h"

/* compiled lua binary */
#include "std.lua.h"


void lj_file_new(upd_file_t* stf, upd_file_t* f) {
  lj_stream_t* st = stf->ctx;
  lua_State*   L  = st->L;

  if (HEDLEY_UNLIKELY(f == NULL)) {
    lua_pushnil(L);
    return;
  }

  upd_file_t** udata = lua_newuserdata(L, sizeof(f));
  *udata = f;

  lua_getfield(L, LUA_REGISTRYINDEX, "std_File");
  lua_setmetatable(L, -2);

  if (HEDLEY_UNLIKELY(!upd_array_insert(&st->files, udata, SIZE_MAX))) {
    *udata = NULL;
    luaL_error(L, "file list insertion failure");
    return;
  }

  /* having self reference causes refcnt circulation */
  if (HEDLEY_LIKELY(f != stf)) {
    upd_file_ref(f);
  }
}


void lj_lock_new(upd_file_t* stf, upd_file_lock_t* k) {
  lj_stream_t* st = stf->ctx;
  lua_State*   L  = st->L;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_free(&k);
    lua_pushnil(L);
    return;
  }

  upd_file_lock_t** udata = lua_newuserdata(L, sizeof(k));
  *udata = k;

  lua_getfield(L, LUA_REGISTRYINDEX, "std_Lock");
  lua_setmetatable(L, -2);

  if (HEDLEY_UNLIKELY(!upd_array_insert(&st->locks, udata, SIZE_MAX))) {
    upd_free(&k);
    *udata = NULL;
    luaL_error(L, "lock insertion failure");
    return;
  }
}


lj_promise_t* lj_promise_new(upd_file_t* stf) {
  lj_stream_t* st = stf->ctx;
  lua_State*   L  = st->L;

  lj_promise_t* pro = lua_newuserdata(L, sizeof(*pro));

  lua_getfield(L, LUA_REGISTRYINDEX, "std_Promise");
  lua_setmetatable(L, -2);

  *pro = (lj_promise_t) {
    .stream   = stf,
    .registry = {
      .result = LUA_REFNIL,
    },
  };
  upd_file_ref(stf);

  lua_pushvalue(L, -1);
  pro->registry.self = luaL_ref(L, LUA_REGISTRYINDEX);
  return pro;
}

int lj_promise_push_result(lj_promise_t* pro) {
  upd_file_t*  stf = pro->stream;
  lj_stream_t* st  = stf->ctx;
  lua_State*   L   = st->L;

  lua_rawgeti(L, LUA_REGISTRYINDEX, pro->registry.result);
  if (HEDLEY_UNLIKELY(lua_istable(L, -1))) {
    for (int i = 1; ; ++i) {
      lua_rawgeti(L, -i, i);
      if (HEDLEY_UNLIKELY(lua_isnil(L, -1))) {
        lua_pop(L, 1);
        return i-1;
      }
    }
  }
  return 1;
}

void lj_promise_finalize(lj_promise_t* pro, bool ok) {
  upd_file_t*  stf = pro->stream;
  lj_stream_t* st  = stf->ctx;

  assert(!pro->done);
  pro->done  = true;
  pro->error = !ok;

  if (HEDLEY_UNLIKELY(pro == st->pending)) {
    lj_stream_resume(stf);
  }
  luaL_unref(st->L, LUA_REGISTRYINDEX, pro->registry.self);
  pro->registry.self = LUA_REFNIL;

  upd_file_unref(stf);
}


static int assert_(lua_State* L) {
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    if (HEDLEY_UNLIKELY(!lua_toboolean(L, i))) {
      return luaL_error(L, "assertion failure at %d-th param", i);
    }
  }
  return n;
}

static int print_(lua_State* L) {
  upd_iso_t* iso = lua_touserdata(L, lua_upvalueindex(1));
  upd_iso_msgf(iso, "%s", luaL_checkstring(L, 1));
  return 0;
}


static void file_lock_cb_(upd_file_lock_t* k) {
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
static int file_lock_(lua_State* L) {
  upd_file_t* stf = lj_stream_get(L);

  const size_t n = lua_gettop(L);
  if (HEDLEY_UNLIKELY(n > 1)) {
    return luaL_error(L, "invalid args");
  }
  upd_file_t* f  = *(void**) lua_touserdata(L, lua_upvalueindex(1));
  const bool  ex = lua_toboolean(L, lua_upvalueindex(2));

  if (HEDLEY_UNLIKELY(stf == f)) {
    return luaL_error(L, "self lock is not allowed");
  }

  const lua_Integer timeout = lua_tointeger(L, 1);
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
    .cb      = file_lock_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(k))) {
    lj_promise_finalize(pro, false);
    goto EXIT;
  }

EXIT:
  lua_pushvalue(L, index);
  return 1;
}

static int file_index_(lua_State* L) {
  upd_file_t* stf = lj_stream_get(L);

  upd_file_t* f = *(void**) lua_touserdata(L, 1);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    return 0;
  }

  const char* key = luaL_checkstring(L, 2);
  if (HEDLEY_UNLIKELY(utf8cmp(key, "backend") == 0)) {
    lj_file_new(stf, f->backend);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "npath") == 0)) {
    lua_pushstring(L, (char*) f->npath);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "param") == 0)) {
    lua_pushstring(L, (char*) f->param);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "path") == 0)) {
    lua_pushstring(L, (char*) f->path);
    return 1;
  }

  if (HEDLEY_UNLIKELY(utf8cmp(key, "lock") == 0)) {
    lua_pushvalue(L, 1);
    lua_pushboolean(L, false);
    lua_pushcclosure(L, file_lock_, 2);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "lockEx") == 0)) {
    lua_pushvalue(L, 1);
    lua_pushboolean(L, true);
    lua_pushcclosure(L, file_lock_, 2);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "watch") == 0)) {
    return luaL_error(L, "NotImplemented");
  }
  return luaL_error(L, "unknown field %s", key);
}

static int file_gc_(lua_State* L) {
  upd_file_t** udata = lua_touserdata(L, 1);
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return 0;
  }

  upd_file_t*  stf = lj_stream_get(L);
  lj_stream_t* st  = stf->ctx;
  upd_array_find_and_remove(&st->files, udata);
  upd_file_unref(*udata);
  return 0;
}


static int lock_unlock_(lua_State* L) {
  if (HEDLEY_UNLIKELY(lua_gettop(L) != 0)) {
    return luaL_error(L, "invalid args");
  }
  upd_file_lock_t** udata = lua_touserdata(L, lua_upvalueindex(1));
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return 0;
  }
  upd_file_unlock(*udata);
  upd_free(&*udata);
  return 0;
}

static int lock_index_(lua_State* L) {
  upd_file_t* stf = lj_stream_get(L);

  upd_file_lock_t* k = *(void**) lua_touserdata(L, 1);
  if (HEDLEY_UNLIKELY(k == NULL)) {
    return 0;
  }

  const char* key = luaL_checkstring(L, 2);
  if (HEDLEY_UNLIKELY(utf8cmp(key, "file") == 0)) {
    lj_file_new(stf, k->file);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "unlock") == 0)) {
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, lock_unlock_, 1);
    return 1;
  }
  return luaL_error(L, "unknown field %s", key);
}

static int lock_gc_(lua_State* L) {
  upd_file_lock_t** udata = lua_touserdata(L, 1);
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return 0;
  }
  upd_file_t*  stf = lj_stream_get(L);
  lj_stream_t* st  = stf->ctx;
  upd_array_find_and_remove(&st->locks, udata);
  upd_free(&*udata);
  return 0;
}


static int promise_result_(lua_State* L) {
  lj_promise_t* pro = lua_touserdata(L, lua_upvalueindex(1));

  if (HEDLEY_UNLIKELY(lua_gettop(L) > 0)) {
    return luaL_error(L, "Promise.result() takes no args");
  }
  return lj_promise_push_result(pro);
}

static int promise_await_(lua_State* L) {
  lj_promise_t* pro = lua_touserdata(L, lua_upvalueindex(1));
  upd_file_t*   stf = pro->stream;
  lj_stream_t*  st  = stf->ctx;

  if (HEDLEY_UNLIKELY(lua_gettop(L) > 0)) {
    return luaL_error(L, "Promise.await() takes no args");
  }
  if (!pro->done) {
    st->state   = LJ_STREAM_PENDING_PROMISE;
    st->pending = pro;
    return lua_yield(L, 0);
  }
  return lj_promise_push_result(pro);
}

static int promise_index_(lua_State* L) {
  lj_promise_t* pro = lua_touserdata(L, 1);

  const char* key = luaL_checkstring(L, 2);
  if (HEDLEY_UNLIKELY(utf8cmp(key, "result") == 0)) {
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, promise_result_, 1);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "await") == 0)) {
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, promise_await_, 1);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "done") == 0)) {
    lua_pushboolean(L, pro->done);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "error") == 0)) {
    lua_pushboolean(L, pro->error);
    return 1;
  }
  return luaL_error(L, "unknown field %s", key);
}

static int promise_gc_(lua_State* L) {
  lj_promise_t* pro = lua_touserdata(L, 1);
  luaL_unref(L, LUA_REGISTRYINDEX, pro->registry.result);
  return 0;
}


void lj_std_register(lua_State* L, upd_iso_t* iso) {
  *(void**) lua_newuserdata(L, sizeof(iso)) = iso;
  lua_setfield(L, LUA_REGISTRYINDEX, "std");

  const int ret = luaL_loadbuffer(
    L, (char*) std_lua, std_lua_len, "##BUILT-IN LUA SCRIPT##");
  if (HEDLEY_UNLIKELY(ret != 0)) {
    fprintf(stderr, "built-in script syntax error: %s\n", lua_tostring(L, -1));
    fflush(stderr);
    assert(false);
  }

  lua_createtable(L, 0, 0);
  {
    lua_getfield(L, LUA_REGISTRYINDEX, "std");
    lua_setfield(L, -2, "std");
  }
  lua_setfenv(L, -2);
  lua_call(L, 0, 1);
  const int std = lua_gettop(L);

  lua_getfield(L, LUA_REGISTRYINDEX, "std");
  {
    lua_createtable(L, 0, 0);
    {
      lua_createtable(L, 0, 0);
      {
        lua_pushcfunction(L, assert_);
        lua_setfield(L, -2, "assert");

        lua_pushlightuserdata(L, iso);
        lua_pushcclosure(L, print_, 1);
        lua_setfield(L, -2, "print");

        lua_create_(L);
        lua_setfield(L, -2, "lua");

        path_create_(L, iso);
        lua_setfield(L, -2, "path");

        mpk_create_(L, std);
        lua_setfield(L, -2, "msgpack");
      }
      lua_setfield(L, -2, "__index");

      lua_pushcfunction(L, lj_lua_immutable_newindex);
      lua_setfield(L, -2, "__newindex");
    }
    lua_setmetatable(L, -2);
  }
  lua_pop(L, 1);

  lua_createtable(L, 0, 0);
  {
    lua_pushcfunction(L, file_index_);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lj_lua_immutable_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, file_gc_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_File");

  lua_createtable(L, 0, 0);
  {
    lua_pushcfunction(L, lock_index_);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lj_lua_immutable_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, lock_gc_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_Lock");

  lua_createtable(L, 0, 0);
  {
    lua_pushcfunction(L, promise_index_);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, promise_gc_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_Promise");

  lua_createtable(L, 0, 0);
  {
    lua_pushcfunction(L, mpk_packer_index_);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, mpk_packer_gc_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_mpk_Packer");

  lua_createtable(L, 0, 0);
  {
    lua_pushcfunction(L, mpk_unpacker_index_);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, mpk_unpacker_gc_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_mpk_Unpacker");

  lua_pop(L, 1);  /* pop std function table */
}
