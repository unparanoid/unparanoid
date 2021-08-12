#include "common.h"


static
bool
dev_init_(
  upd_file_t* f);

static
void
dev_deinit_(
  upd_file_t* f);

static
bool
dev_handle_(
  upd_req_t* req);

const upd_driver_t lj_dev = {
  .name = (uint8_t*) "upd.luajit.dev",
  .cats = (upd_req_cat_t[]) {
    0,
  },
  .init   = dev_init_,
  .deinit = dev_deinit_,
  .handle = dev_handle_,
};


static bool dev_init_(upd_file_t* f) {
  lj_dev_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }

  ctx->L = luaL_newstate();
  if (HEDLEY_UNLIKELY(ctx->L == NULL)) {
    upd_free(&ctx);
    return false;
  }
  f->ctx = ctx;
  lj_std_register(ctx->L, f->iso);
  return true;
}

static void dev_deinit_(upd_file_t* f) {
  lj_dev_t*  ctx = f->ctx;
  lua_State* L   = ctx->L;

# if !defined(NDEBUG)
    lua_pushnil(L);
    while (lua_next(L, LUA_REGISTRYINDEX) != 0) {
      lua_pushvalue(L, -2);
      upd_iso_msgf(f->iso, "%s - %s\n",
          lua_tostring(L, -1),
          lua_typename(L, lua_type(L, -2)));
      lua_pop(L, 2);
    }
# endif

  lua_close(L);
  upd_free(&ctx);
}

static bool dev_handle_(upd_req_t* req) {
  req->result = UPD_REQ_INVALID;
  return false;
}
