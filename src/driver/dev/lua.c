#include "common.h"


/* ---- list of lua identifiers ----
 * [Device Defined]
 *   Iso, iso, File, Req
 * [Program Stream Defined]
 *   ProgramStream
 */


typedef enum lua_req_type_t_ {
  LUA_REQ_STANDARD_,
  LUA_REQ_PATHFIND_,
} lua_req_type_t_;

typedef struct lua_req_t_ {
  union {
    upd_req_t          std;
    upd_req_pathfind_t pf;
  };
  lua_req_type_t_ type;

  lua_State* lua;
  int        registry;

  unsigned error : 1;
  unsigned done  : 1;
} lua_req_t_;


static
bool
lua_init_(
  upd_file_t* f);

static
void
lua_deinit_(
  upd_file_t* f);

static
bool
lua_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_dev_lua = {
  .name = (uint8_t*) "upd.dev.lua",
  .cats = (upd_req_cat_t[]) {
    0,
  },
  .init   = lua_init_,
  .deinit = lua_deinit_,
  .handle = lua_handle_,
};


static
int
lua_frozen_newindex_(
  lua_State* lua);

static
lua_CFunction
lua_find_reg_(
  luaL_Reg*   regs,
  const char* key);


static
void
lua_iso_build_class_(
  lua_State* lua,
  upd_iso_t* iso);


static
void
lua_file_build_class_(
  lua_State* lua);

static
void
lua_file_new_(
  lua_State*  lua,
  upd_file_t* f);


static
void
lua_req_build_class_(
  lua_State* lua);

static
lua_req_t_*
lua_req_new_(
  lua_State* lua,
  size_t     additional);

static
int
lua_req_push_result_(
  lua_req_t_* req,
  lua_State*  lua);

static
void
lua_req_finalize_(
  lua_req_t_* req,
  bool        ok);


static
void
lua_req_pathfind_cb_(
  upd_req_pathfind_t* pf);


static bool lua_init_(upd_file_t* f) {
  lua_State* lua = luaL_newstate();
  if (HEDLEY_UNLIKELY(lua == NULL)) {
    return false;
  }

  lua_iso_build_class_(lua, f->iso);
  lua_file_build_class_(lua);
  lua_req_build_class_(lua);

  f->ctx = lua;
  return true;
}

static void lua_deinit_(upd_file_t* f) {
  lua_State* lua = f->ctx;
  lua_close(lua);
}

static bool lua_handle_(upd_req_t* req) {
  (void) req;
  return false;
}


static int lua_frozen_newindex_(lua_State* lua) {
  return luaL_error(lua, "tried to modify immutable object");
}

static lua_CFunction lua_find_reg_(luaL_Reg* regs, const char* key) {
  for (size_t i = 0; regs[i].name; ++i) {
    if (HEDLEY_UNLIKELY(utf8cmp(key, regs[i].name) == 0)) {
      return regs[i].func;
    }
  }
  return NULL;
}


static int lua_iso_msg_(lua_State* lua) {
  upd_iso_t* iso = *(void**) luaL_checkudata(lua, lua_upvalueindex(1), "Iso");

  const int n = lua_gettop(lua);
  if (HEDLEY_UNLIKELY(n < 1)) {
    return luaL_error(lua, "Iso.msg requires one or more parameters");
  }
  for (int i = 1; i <= n; ++i) {
    const int t = lua_type(lua, i);
    switch (t) {
    case LUA_TNIL:
      upd_iso_msgf(iso, "(nil)");
      break;
    case LUA_TNUMBER:
      upd_iso_msgf(iso, "%g", lua_tonumber(lua, i));
      break;
    case LUA_TBOOLEAN:
      upd_iso_msgf(iso, lua_toboolean(lua, i)? "true": "false");
      break;
    case LUA_TSTRING: {
      size_t len;
      const char* s = lua_tolstring(lua, i, &len);
      upd_iso_msgf(iso, "%.*s", (int) len, s);
    } break;
    case LUA_TTABLE:
      upd_iso_msgf(iso, "(table)");
      break;
    case LUA_TFUNCTION:
      upd_iso_msgf(iso, "(function)");
      break;
    case LUA_TUSERDATA:
      upd_iso_msgf(iso, "(userdata)");
      break;
    case LUA_TTHREAD:
      upd_iso_msgf(iso, "(thread)");
      break;
    case LUA_TLIGHTUSERDATA:
      upd_iso_msgf(iso, "(lightuserdata)");
      break;
    default:
      upd_iso_msgf(iso, "(unknown)");
    }
  }
  return 0;
}
static int lua_iso_pathfind_(lua_State* lua) {
  upd_iso_t* iso = *(void**) luaL_checkudata(lua, lua_upvalueindex(1), "Iso");

  const size_t n = lua_gettop(lua);
  if (HEDLEY_UNLIKELY(n != 1)) {
    return luaL_error(lua, "Iso.pathfind() takes just one argument");
  }

  size_t len;
  const char* path = luaL_checklstring(lua, 1, &len);;

  lua_req_t_* req = lua_req_new_(lua, len);

  req->type = LUA_REQ_PATHFIND_;
  req->pf   = (upd_req_pathfind_t) {
    .iso  = iso,
    .path = utf8ncpy(req+1, path, len),
    .len  = len,
    .cb   = lua_req_pathfind_cb_,
  };
  upd_req_pathfind(&req->pf);
  return 1;
}
static int lua_iso_index_(lua_State* lua) {
  upd_iso_t* iso = *(void**) lua_touserdata(lua, 1);

  const char* key = luaL_checkstring(lua, 2);
  if (utf8cmp(key, "now") == 0) {
    lua_pushinteger(lua, upd_iso_now(iso));
    return 1;
  }

  lua_CFunction f = lua_find_reg_((luaL_Reg[]) {
      { "msg",      lua_iso_msg_ },
      { "pathfind", lua_iso_pathfind_ },
      { NULL, NULL }
    }, key);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    return luaL_error(lua, "unknown field Iso.%s", key);
  }
  lua_pushvalue(lua, 1);
  lua_pushcclosure(lua, f, 1);
  return 1;
}
static void lua_iso_build_class_(lua_State* lua, upd_iso_t* iso) {
  upd_iso_t** ptr = lua_newuserdata(lua, sizeof(iso));
  *ptr = iso;

  luaL_newmetatable(lua, "Iso");

  lua_pushcfunction(lua, lua_iso_index_);
  lua_setfield(lua, -2, "__index");

  lua_pushcfunction(lua, lua_frozen_newindex_);
  lua_setfield(lua, -2, "__newindex");

  lua_pushboolean(lua, false);
  lua_setfield(lua, -2, "__metatable");

  lua_setmetatable(lua, -2);
  lua_setglobal(lua, "iso");
}


static int lua_file_index_(lua_State* lua) {
  upd_file_t* f = *(void**) lua_touserdata(lua, 1);

  const char* key = luaL_checkstring(lua, 2);
  if (utf8cmp(key, "id") == 0) {
    lua_pushinteger(lua, f->id);
    return 1;
  }
  if (utf8cmp(key, "npath") == 0) {
    if (f->npath) {
      lua_pushstring(lua, (char*) f->npath);
    } else {
      lua_pushnil(lua);
    }
    return 1;
  }
  if (utf8cmp(key, "lastUpdate") == 0) {
    lua_pushinteger(lua, f->last_update);
    return 1;
  }
  if (utf8cmp(key, "refcnt") == 0) {
    lua_pushinteger(lua, f->refcnt);
    return 1;
  }

  lua_CFunction fun = lua_find_reg_((luaL_Reg[]) {
      { NULL, NULL }
    }, key);
  if (HEDLEY_UNLIKELY(fun == NULL)) {
    return luaL_error(lua, "unknown field File.%s", key);
  }
  lua_pushvalue(lua, 1);
  lua_pushcclosure(lua, fun, 1);
  return 1;
}
static int lua_file_gc_(lua_State* lua) {
  upd_file_t* f = *(void**) lua_touserdata(lua, 1);
  upd_file_unref(f);
  return 0;
}
static void lua_file_build_class_(lua_State* lua) {
  luaL_newmetatable(lua, "File");

  lua_pushcfunction(lua, lua_file_index_);
  lua_setfield(lua, -2, "__index");

  lua_pushcfunction(lua, lua_file_gc_);
  lua_setfield(lua, -2, "__gc");

  lua_pushboolean(lua, false);
  lua_setfield(lua, -2, "__metatable");

  lua_pop(lua, 1);
}

void lua_file_new_(lua_State* lua, upd_file_t* f) {
  upd_file_t** ptr = lua_newuserdata(lua, sizeof(f));
  *ptr = f;

  lua_getfield(lua, LUA_REGISTRYINDEX, "File");
  lua_setmetatable(lua, -2);

  upd_file_ref(f);
}


static int lua_req_await_(lua_State* lua) {
  lua_req_t_* req;
  bool        done;
BEGIN:
  req = luaL_checkudata(lua, lua_upvalueindex(1), "Req");

  if (HEDLEY_UNLIKELY(lua_gettop(lua) >= 1)) {
    return luaL_error(lua, "Req.await() doesn't need any parameters");
  }

  done = req->done;
  if (HEDLEY_LIKELY(!done)) {
    return lua_yield(lua, 0);
  }
  if (HEDLEY_UNLIKELY(!done)) {
    goto BEGIN;
  }
  return lua_req_push_result_(req, lua);
}
static int lua_req_index_(lua_State* lua) {
  lua_req_t_* req = luaL_checkudata(lua, 1, "Req");

  const char* key = luaL_checkstring(lua, 2);
  if (HEDLEY_UNLIKELY(utf8cmp(key, "done") == 0)) {
    lua_pushboolean(lua, req->done);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "error") == 0)) {
    lua_pushboolean(lua, req->error);
    return 1;
  }
  if (HEDLEY_UNLIKELY(utf8cmp(key, "result") == 0)) {
    return lua_req_push_result_(req, lua);
  }

  lua_CFunction f = lua_find_reg_((luaL_Reg[]) {
      { "await",  lua_req_await_ },
      { NULL, NULL }
    }, key);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    return luaL_error(lua, "unknown field Req.%s", key);
  }
  lua_pushvalue(lua, 1);
  lua_pushcclosure(lua, f, 1);
  return 1;
}
static void lua_req_build_class_(lua_State* lua) {
  luaL_newmetatable(lua, "Req");

  lua_pushcfunction(lua, lua_req_index_);
  lua_setfield(lua, -2, "__index");

  lua_pushboolean(lua, false);
  lua_setfield(lua, -2, "__metatable");

  lua_pop(lua, 1);
}

static lua_req_t_* lua_req_new_(lua_State* lua, size_t additional) {
  lua_req_t_* req = (void*) lua_newuserdata(lua, sizeof(*req)+additional);

  lua_pushvalue(lua, -1);
  const int reg = luaL_ref(lua, LUA_REGISTRYINDEX);
  *req = (lua_req_t_) {
    .lua      = lua,
    .registry = reg,
  };

  lua_getfield(lua, LUA_REGISTRYINDEX, "Req");
  lua_setmetatable(lua, -2);
  return req;
}

static int lua_req_push_result_(lua_req_t_* req, lua_State* lua) {
  if (HEDLEY_UNLIKELY(!req->done)) {
    return luaL_error(lua, "req is not handled yet");
  }
  const upd_req_t*          std = &req->std;
  const upd_req_pathfind_t* pf  = &req->pf;

  switch (req->type) {
  case LUA_REQ_STANDARD_:
    switch (std->type) {
    case UPD_REQ_DIR_ACCESS:
      lua_createtable(lua, 0, 0);

      lua_pushboolean(lua, std->dir.access.list);
      lua_setfield(lua, -2, "list");

      lua_pushboolean(lua, std->dir.access.find);
      lua_setfield(lua, -2, "find");

      lua_pushboolean(lua, std->dir.access.add);
      lua_setfield(lua, -2, "add");

      lua_pushboolean(lua, std->dir.access.rm);
      lua_setfield(lua, -2, "rm");
      return 1;

    default:
      return luaL_error(lua, "unknown request type");
    }

  case LUA_REQ_PATHFIND_:
    if (HEDLEY_UNLIKELY(pf->len)) {
      lua_pushnil(lua);
      lua_pushlstring(lua, (char*) pf->path, pf->len);
      lua_file_new_(lua, pf->base);
    } else {
      lua_file_new_(lua, pf->base);
      lua_pushlstring(lua, "", 0);
      lua_pushvalue(lua, -2);
    }
    return 3;
  }
  luaL_error(lua, "fatal error in Req.result");
  HEDLEY_UNREACHABLE();
}

static void lua_req_finalize_(lua_req_t_* req, bool ok) {
  assert(!req->done);

  lua_State* lua = req->lua;

  req->error = !ok;
  req->done  = true;

  luaL_unref(lua, LUA_REGISTRYINDEX, req->registry);
}


static void lua_req_pathfind_cb_(upd_req_pathfind_t* pf) {
  lua_req_finalize_((void*) pf, true);
}
