#pragma once


static int path_normalize_(lua_State* L) {
  upd_iso_t* iso = lua_touserdata(L, lua_upvalueindex(1));

  size_t len;
  const uint8_t* src = (void*) luaL_checklstring(L, 1, &len);

  len = utf8nsize_lazy(src, len);
  if (HEDLEY_UNLIKELY(len == 0)) {
    lua_pushstring(L, "");
    return 1;
  }

  uint8_t* dst = upd_iso_stack(iso, len);
  if (HEDLEY_UNLIKELY(dst == NULL)) {
    lua_pushnil(L);
    return 1;
  }
  utf8ncpy(dst, src, len);

  len = upd_path_normalize(dst, len);

  lua_pushlstring(L, (char*) dst, len);
  upd_iso_unstack(iso, dst);
  return 1;
}

static int path_validate_name_(lua_State* L) {
  size_t len;
  const uint8_t* name = (void*) luaL_checklstring(L, 1, &len);
  len = utf8nsize_lazy(name, len);

  lua_pushboolean(L, upd_path_validate_name(name, len));
  return 1;
}

static int path_drop_trailing_slash_(lua_State* L) {
  size_t len;
  const uint8_t* path = (void*) luaL_checklstring(L, 1, &len);
  len = utf8nsize_lazy(path, len);

  len = upd_path_drop_trailing_slash(path, len);

  lua_pushlstring(L, (char*) path, len);
  return 1;
}

static int path_dirname_(lua_State* L) {
  size_t len;
  const uint8_t* path = (void*) luaL_checklstring(L, 1, &len);
  len = utf8nsize_lazy(path, len);

  len = upd_path_dirname(path, len);

  lua_pushlstring(L, (char*) path, len);
  return 1;
}

static int path_basename_(lua_State* L) {
  size_t len;
  const uint8_t* path = (void*) luaL_checklstring(L, 1, &len);
  len = utf8nsize_lazy(path, len);

  path = upd_path_basename(path, &len);

  lua_pushlstring(L, (char*) path, len);
  return 1;
}
