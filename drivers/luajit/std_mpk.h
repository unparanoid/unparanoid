#pragma once

typedef struct mpk_packer_t_ {
  msgpack_packer  pk;
  msgpack_sbuffer buf;
} mpk_packer_t_;

typedef struct mpk_unpacker_t_ {
  msgpack_unpacker                upk;
  upd_array_of(msgpack_unpacked*) upkd;

  unsigned broken : 1;
} mpk_unpacker_t_;


static int mpk_packer_new_(lua_State* L) {
  mpk_packer_t_* pk = lua_newuserdata(L, sizeof(*pk));

  lua_getfield(L, LUA_REGISTRYINDEX, "std_mpk_Packer");
  lua_setmetatable(L, -2);

  *pk = (mpk_packer_t_) {0};

  msgpack_sbuffer_init(&pk->buf);
  msgpack_packer_init(&pk->pk, &pk->buf, msgpack_sbuffer_write);

  return 1;
}

static int mpk_packer_take_(lua_State* L) {
  mpk_packer_t_* pk = luaL_checkudata(L, 1, "std_mpk_Packer");

  lua_pushlstring(L, pk->buf.data, pk->buf.size);
  msgpack_sbuffer_clear(&pk->buf);
  return 1;
}

static bool mpk_packer_pack_proc_(lua_State* L, int i, msgpack_packer* pk) {
  const int t = lua_type(L, i);

  switch (t) {
  case LUA_TNUMBER: {
    const lua_Number f = lua_tonumber(L, i);
    if (f == (intmax_t) f) {
      return !msgpack_pack_int64(pk, f);
    } else {
      return !msgpack_pack_double(pk, f);
    }
  }

  case LUA_TBOOLEAN:
    return !upd_msgpack_pack_bool(pk, lua_toboolean(L, i));

  case LUA_TSTRING: {
    size_t len;
    const char* s = lua_tolstring(L, i, &len);
    return !msgpack_pack_str_with_body(pk, s, len);
  }

  case LUA_TTABLE: {
    size_t n   = 0;
    bool   arr = true;

    lua_pushnil(L);
    while (lua_next(L, i) != 0) {
      arr = arr && lua_isnumber(L, -2);
      if (arr) {
        const lua_Number f = lua_tonumber(L, -2);
        arr = f == (intmax_t) f;
      }
      ++n;
      lua_pop(L, 1);
    }
    arr = arr && n;

    if (HEDLEY_UNLIKELY((arr? msgpack_pack_array: msgpack_pack_map)(pk, n))) {
      return false;
    }
    lua_pushnil(L);
    while (lua_next(L, i) != 0) {
      const int i = lua_gettop(L);
      if (!arr) {
        if (HEDLEY_UNLIKELY(!mpk_packer_pack_proc_(L, i-1, pk))) {
          return false;
        }
      }
      if (HEDLEY_UNLIKELY(!mpk_packer_pack_proc_(L, i, pk))) {
        return false;
      }
      lua_pop(L, 1);
    }
  } return true;

  default:
    return !msgpack_pack_nil(pk);
  }
}
static int mpk_packer_pack_(lua_State* L) {
  mpk_packer_t_* pk = luaL_checkudata(L, 1, "std_mpk_Packer");

  const int n = lua_gettop(L);
  for (int i = 2; i <= n; ++i) {
    if (HEDLEY_UNLIKELY(!mpk_packer_pack_proc_(L, i, &pk->pk))) {
      return luaL_error(L,
        "pack failure (allocation failure)");
    }
  }
  return 0;
}

static int mpk_packer_gc_(lua_State* L) {
  mpk_packer_t_* pk = luaL_checkudata(L, 1, "std_mpk_Packer");

  msgpack_sbuffer_destroy(&pk->buf);
  return 0;
}


static int mpk_unpacker_new_(lua_State* L) {
  mpk_unpacker_t_* upk = lua_newuserdata(L, sizeof(*upk));

  lua_getfield(L, LUA_REGISTRYINDEX, "std_mpk_Unpacker");
  lua_setmetatable(L, -2);

  *upk = (mpk_unpacker_t_) {0};

  size_t reserve = 1024;
  if (lua_isnumber(L, 1)) {
    const lua_Number f = lua_tonumber(L, 1);
    if (HEDLEY_UNLIKELY(f <= 0)) {
      return luaL_error(L, "negative or zero reserve size");
    }
    reserve = f;
  }

  if (HEDLEY_UNLIKELY(!msgpack_unpacker_init(&upk->upk, reserve))) {
    return luaL_error(L, "allocation failure");
  }
  return 1;
}

static int mpk_unpacker_drop_(lua_State* L) {
  mpk_unpacker_t_* upk = luaL_checkudata(L, 1, "std_mpk_Unpacker");

  for (size_t i = 0; i < upk->upkd.n; ++i) {
    msgpack_unpacked* upkd = upk->upkd.p[i];
    msgpack_unpacked_destroy(upkd);
    upd_free(&upkd);
  }
  upd_array_clear(&upk->upkd);
  return 0;
}

static void mpk_unpacker_pop_proc_(lua_State* L, const msgpack_object* obj) {
  switch (obj->type) {
  case MSGPACK_OBJECT_BOOLEAN:
    lua_pushboolean(L, obj->via.boolean);
    return;
  case MSGPACK_OBJECT_POSITIVE_INTEGER:
    if (HEDLEY_LIKELY(obj->via.u64 < INT64_MAX)) {
      lua_pushinteger(L, obj->via.u64);
    } else {
      lua_pushnil(L);
    }
    return;
  case MSGPACK_OBJECT_NEGATIVE_INTEGER:
    lua_pushinteger(L, obj->via.i64);
    return;
  case MSGPACK_OBJECT_FLOAT32:
  case MSGPACK_OBJECT_FLOAT64:
    lua_pushnumber(L, obj->via.f64);
    return;
  case MSGPACK_OBJECT_STR:
    lua_pushlstring(L, obj->via.str.ptr, obj->via.str.size);
    return;
  case MSGPACK_OBJECT_ARRAY:
    lua_createtable(L, obj->via.array.size, 0);
    for (size_t i = 0; i < obj->via.array.size; ++i) {
      mpk_unpacker_pop_proc_(L, &obj->via.array.ptr[i]);
      lua_rawseti(L, -2, i+1);
    }
    return;
  case MSGPACK_OBJECT_MAP:
    lua_createtable(L, 0, obj->via.map.size);
    for (size_t i = 0; i < obj->via.map.size; ++i) {
      mpk_unpacker_pop_proc_(L, &obj->via.map.ptr[i].key);
      mpk_unpacker_pop_proc_(L, &obj->via.map.ptr[i].val);
      lua_rawset(L, -3);
    }
    return;
  default:
    lua_pushnil(L);
    return;
  }
}
static int mpk_unpacker_pop_(lua_State* L) {
  mpk_unpacker_t_* upk = luaL_checkudata(L, 1, "std_mpk_Unpacker");

  const lua_Integer n = lua_gettop(L) >= 2? lua_tointeger(L, 2): 1;
  if (HEDLEY_UNLIKELY(n < 0)) {
    return luaL_error(L, "invalid pop count");
  }

  for (lua_Integer i = 0; i < n; ++i) {
    msgpack_unpacked* upkd = upd_array_remove(&upk->upkd, 0);
    if (HEDLEY_UNLIKELY(upkd == NULL)) {
      return i;
    }
    mpk_unpacker_pop_proc_(L, &upkd->data);
    msgpack_unpacked_destroy(upkd);
    upd_free(&upkd);
  }
  return n;
}

static int mpk_unpacker_unpack_(lua_State* L) {
  mpk_unpacker_t_* upk = luaL_checkudata(L, 1, "std_mpk_Unpacker");

  if (HEDLEY_UNLIKELY(upk->broken)) {
    return luaL_error(L, "unpacker is broken");
  }

  const int n = lua_gettop(L);

  size_t sum = 0;
  for (int i = 2; i <= n; ++i) {
    size_t len;
    luaL_checklstring(L, i, &len);
    sum += len;
  }
  if (HEDLEY_UNLIKELY(!msgpack_unpacker_reserve_buffer(&upk->upk, sum))) {
    return luaL_error(L, "unpacker buffer allocation failure");
  }

  for (int i = 2; i <= n; ++i) {
    size_t len;
    const char* buf = luaL_checklstring(L, i, &len);
    memcpy(msgpack_unpacker_buffer(&upk->upk), buf, len);
    msgpack_unpacker_buffer_consumed(&upk->upk, len);
  }

  for (;;) {
    msgpack_unpacked* upkd = NULL;
    if (HEDLEY_UNLIKELY(!upd_malloc(&upkd, sizeof(*upkd)))) {
      return luaL_error(L, "unpacked data allocation failure");
    }
    msgpack_unpacked_init(upkd);

    const int ret = msgpack_unpacker_next(&upk->upk, upkd);
    switch (ret) {
    case MSGPACK_UNPACK_SUCCESS:
      if (HEDLEY_UNLIKELY(!upd_array_insert(&upk->upkd, upkd, SIZE_MAX))) {
        msgpack_unpacked_destroy(upkd);
        upd_free(&upkd);
        return luaL_error(L, "unpacked data insertion failure");
      }
      continue;

    case MSGPACK_UNPACK_CONTINUE:
      msgpack_unpacked_destroy(upkd);
      upd_free(&upkd);
      lua_pushinteger(L, upk->upkd.n);
      return 1;

    case MSGPACK_UNPACK_PARSE_ERROR:
      msgpack_unpacked_destroy(upkd);
      upd_free(&upkd);
      upk->broken = true;
      return 0;
    }
  }
}

static int mpk_unpacker_gc_(lua_State* L) {
  mpk_unpacker_t_* upk = lua_touserdata(L, 1);
  mpk_unpacker_drop_(L);

  msgpack_unpacker_destroy(&upk->upk);
  return 0;
}
