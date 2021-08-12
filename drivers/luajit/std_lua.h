#pragma once


static int lua_set_metatable_(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  lua_setmetatable(L, -2);
  return 1;
}

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


static void lua_create_(lua_State* L) {
  lua_newuserdata(L, 0);
  {
    lua_createtable(L, 0, 0);
    {
      lua_createtable(L, 0, 0);
      {
        lua_pushcfunction(L, lua_set_metatable_);
        lua_setfield(L, -2, "setMetatable");

        lua_pushcfunction(L, lua_pairs_);
        lua_setfield(L, -2, "pairs");
      }
      lua_setfield(L, -2, "__index");

      lua_pushcfunction(L, lj_lua_immutable_newindex);
      lua_setfield(L, -2, "__newindex");
    }
    lua_setmetatable(L, -2);
  }
}
