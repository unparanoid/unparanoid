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
      return luaL_error(L, "assertion failure", i);
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
  const bool ex = lua_toboolean(L, lua_upvalueindex(1));

  upd_file_t* stf = lj_stream_get(L);

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

static int file_npath_(lua_State* L) {
  upd_file_t** udata = luaL_checkudata(L, 1, "std_File");
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return luaL_error(L, "file has been torn down");
  }
  upd_file_t* f = *udata;
  lua_pushlstring(L, (char*) f->npath, f->npathlen);
  return 1;
}

static int file_param_(lua_State* L) {
  upd_file_t** udata = luaL_checkudata(L, 1, "std_File");
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return luaL_error(L, "file has been torn down");
  }
  upd_file_t* f = *udata;
  lua_pushlstring(L, (char*) f->param, f->paramlen);
  return 1;
}

static int file_path_(lua_State* L) {
  upd_file_t** udata = luaL_checkudata(L, 1, "std_File");
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return luaL_error(L, "file has been torn down");
  }
  upd_file_t* f = *udata;
  lua_pushlstring(L, (char*) f->path, f->pathlen);
  return 1;
}

static int file_teardown_(lua_State* L) {
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


static int lock_teardown_(lua_State* L) {
  if (HEDLEY_UNLIKELY(lua_gettop(L) != 0)) {
    return luaL_error(L, "invalid args");
  }
  upd_file_lock_t** udata = luaL_checkudata(L, 1, "std_Lock");
  if (HEDLEY_UNLIKELY(*udata == NULL)) {
    return 0;
  }

  upd_file_t*  stf = lj_stream_get(L);
  lj_stream_t* st  = stf->ctx;
  upd_array_find_and_remove(&st->locks, udata);

  upd_file_unlock(*udata);
  upd_free(&*udata);
  return 0;
}


static int promise_await_(lua_State* L) {
  lj_promise_t* pro = luaL_checkudata(L, 1, "std_Promise");
  upd_file_t*   stf = pro->stream;
  lj_stream_t*  st  = stf->ctx;

  if (!pro->done) {
    st->state   = LJ_STREAM_PENDING_PROMISE;
    st->pending = pro;
    return lua_yield(L, 0);
  }
  return lj_promise_push_result(pro);
}

static int promise_done_(lua_State* L) {
  lj_promise_t* pro = luaL_checkudata(L, 1, "std_Promise");
  lua_pushboolean(L, pro->done);
  return 1;
}

static int promise_error_(lua_State* L) {
  lj_promise_t* pro = luaL_checkudata(L, 1, "std_Promise");
  lua_pushboolean(L, pro->error);
  return 1;
}

static int promise_result_(lua_State* L) {
  lj_promise_t* pro = luaL_checkudata(L, 1, "std_Promise");
  return lj_promise_push_result(pro);
}

static int promise_gc_(lua_State* L) {
  lj_promise_t* pro = luaL_checkudata(L, 1, "std_Promise");
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

        lua_getfield(L, std, "lambda");
        lua_setfield(L, -2, "lambda");

        lua_newuserdata(L, 0);
        {
          lua_createtable(L, 0, 0);
          {
            lua_createtable(L, 0, 0);
            {
              lua_pushinteger(L, LUA_TFUNCTION);
              lua_pushcclosure(L, lua_type_check_, 1);
              lua_setfield(L, -2, "isFunction");

              lua_pushinteger(L, LUA_TNUMBER);
              lua_pushcclosure(L, lua_type_check_, 1);
              lua_setfield(L, -2, "isNumber");

              lua_pushinteger(L, LUA_TSTRING);
              lua_pushcclosure(L, lua_type_check_, 1);
              lua_setfield(L, -2, "isString");

              lua_pushinteger(L, LUA_TTABLE);
              lua_pushcclosure(L, lua_type_check_, 1);
              lua_setfield(L, -2, "isTable");

              lua_pushcfunction(L, lua_pairs_);
              lua_setfield(L, -2, "pairs");

              lua_pushcfunction(L, lua_set_metatable_);
              lua_setfield(L, -2, "setMetatable");

              lua_pushcfunction(L, lua_type_);
              lua_setfield(L, -2, "type");
            }
            lua_setfield(L, -2, "__index");
          }
          lua_setmetatable(L, -2);
        }
        lua_setfield(L, -2, "lua");

        lua_newuserdata(L, 0);
        {
          lua_createtable(L, 0, 0);
          {
            lua_createtable(L, 0, 0);
            {
              lua_pushcfunction(L, mpk_packer_new_);
              lua_setfield(L, -2, "packer");

              lua_pushcfunction(L, mpk_unpacker_new_);
              lua_setfield(L, -2, "unpacker");

              lua_getfield(L, std, "mpk_unpack");
              lua_setfield(L, -2, "unpack");

              lua_getfield(L, std, "mpk_pack");
              lua_setfield(L, -2, "pack");
            }
            lua_setfield(L, -2, "__index");
          }
          lua_setmetatable(L, -2);
        }
        lua_setfield(L, -2, "msgpack");

        lua_newuserdata(L, 0);
        {
          lua_createtable(L, 0, 0);
          {
            lua_createtable(L, 0, 0);
            {
              lua_pushlightuserdata(L, iso);
              lua_pushcclosure(L, path_normalize_, 1);
              lua_setfield(L, -2, "normalize");

              lua_pushcfunction(L, path_validate_name_);
              lua_setfield(L, -2, "validateName");

              lua_pushcfunction(L, path_drop_trailing_slash_);
              lua_setfield(L, -2, "dropTrailingSlash");

              lua_pushcfunction(L, path_dirname_);
              lua_setfield(L, -2, "dirname");

              lua_pushcfunction(L, path_basename_);
              lua_setfield(L, -2, "basename");
            }
            lua_setfield(L, -2, "__index");
          }
          lua_setmetatable(L, -2);
        }
        lua_setfield(L, -2, "path");

        lua_pushlightuserdata(L, iso);
        lua_pushcclosure(L, print_, 1);
        lua_setfield(L, -2, "print");

        lua_newuserdata(L, 0);
        {
          lua_createtable(L, 0, 0);
          {
            lua_createtable(L, 0, 0);
            {
              lua_getfield(L, std, "trait_number");
              lua_setfield(L, -2, "number");
            }
            lua_setfield(L, -2, "__index");
          }
          lua_setmetatable(L, -2);
        }
        lua_setfield(L, -2, "trait");
      }
      lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, -2);
  }
  lua_pop(L, 1);

  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 0);
    {
      lua_pushboolean(L, false);
      lua_pushcclosure(L, file_lock_, 1);
      lua_setfield(L, -2, "lock");

      lua_pushboolean(L, true);
      lua_pushcclosure(L, file_lock_, 1);
      lua_setfield(L, -2, "lockEx");

      lua_pushcfunction(L, file_npath_);
      lua_setfield(L, -2, "npath");

      lua_pushcfunction(L, file_param_);
      lua_setfield(L, -2, "param");

      lua_pushcfunction(L, file_path_);
      lua_setfield(L, -2, "path");

      lua_pushcfunction(L, file_teardown_);
      lua_setfield(L, -2, "teardown");
    }
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, file_teardown_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_File");

  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 0);
    {
      lua_pushcfunction(L, lock_teardown_);
      lua_setfield(L, -2, "teardown");
    }
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lock_teardown_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_Lock");

  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 0);
    {
      lua_pushcfunction(L, promise_await_);
      lua_setfield(L, -2, "await");

      lua_pushcfunction(L, promise_done_);
      lua_setfield(L, -2, "done");

      lua_pushcfunction(L, promise_error_);
      lua_setfield(L, -2, "error");

      lua_pushcfunction(L, promise_result_);
      lua_setfield(L, -2, "result");
    }
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, promise_gc_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_Promise");

  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 0);
    {
      lua_pushcfunction(L, mpk_packer_pack_);
      lua_setfield(L, -2, "pack");

      lua_pushcfunction(L, mpk_packer_take_);
      lua_setfield(L, -2, "take");
    }
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, mpk_packer_gc_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_mpk_Packer");

  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 0);
    {
      lua_pushcfunction(L, mpk_unpacker_drop_);
      lua_setfield(L, -2, "drop");

      lua_pushcfunction(L, mpk_unpacker_pop_);
      lua_setfield(L, -2, "pop");

      lua_pushcfunction(L, mpk_unpacker_unpack_);
      lua_setfield(L, -2, "unpack");
    }
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, mpk_unpacker_gc_);
    lua_setfield(L, -2, "__gc");
  }
  lua_setfield(L, LUA_REGISTRYINDEX, "std_mpk_Unpacker");

  lua_pop(L, 1);  /* pop std function table */
}