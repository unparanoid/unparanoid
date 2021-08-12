#pragma once


static int lua_pairs_internal_(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  if (HEDLEY_LIKELY(lua_next(L, 1))) {
    return 2;
  }
  return 0;
}
static int lua_pairs_(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushcfunction(L, lua_pairs_internal_);
  lua_pushvalue(L, 1);
  lua_pushnil(L);
  return 3;
}

static int lua_type_(lua_State* L) {
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    lua_pushstring(L, lua_typename(L, lua_type(L, i)));
  }
  return n;
}
static int lua_type_check_(lua_State* L) {
  const int t = lua_tointeger(L, lua_upvalueindex(1));

  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    if (HEDLEY_UNLIKELY(lua_type(L, i) != t)) {
      lua_pushboolean(L, false);
      return 1;
    }
  }
  lua_pushboolean(L, true);
  return 1;
}

static int lua_set_metatable_(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);

  lua_getfield(L, 2, "__gc");
  if (HEDLEY_UNLIKELY(!lua_isnil(L, -1))) {
    return luaL_error(L, "do not use __gc");
  }
  lua_pop(L, 1);

  lua_setmetatable(L, -2);
  return 1;
}
