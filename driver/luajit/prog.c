#include "common.h"


#define LOG_PREFIX_ "upd.luajit: "


static
bool
prog_init_(
  upd_file_t* f);

static
void
prog_deinit_(
  upd_file_t* f);

static
bool
prog_handle_(
  upd_req_t* req);

static
upd_file_t*
prog_exec_(
  upd_file_t* f);

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
prog_logf_(
  upd_file_t* f,
  const char* fmt,
  ...);

const upd_driver_t lj_prog = {
  .name = (uint8_t*) "upd.luajit",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_PROG,
    0,
  },
  .init   = prog_init_,
  .deinit = prog_deinit_,
  .handle = prog_handle_,
};


static
void
compile_finalize_(
  lj_compile_t* cp);


static
void
prog_lock_self_cb_(
  upd_file_lock_t* k);

static
void
prog_pathfind_dev_cb_(
  upd_pathfind_t* pf);

static
void
prog_watch_bin_cb_(
  upd_file_watch_t* w);

static
void
prog_compile_cb_(
  lj_compile_t* cp);


static
void
compile_lock_bin_cb_(
  upd_file_lock_t* k);

static
void
compile_read_bin_cb_(
  upd_req_t* req);


bool lj_compile(lj_compile_t* cp) {
  upd_file_t* f   = cp->prog;
  lj_prog_t*  ctx = f->ctx;
  lj_dev_t*   dev = ctx->dev->ctx;

  cp->L = dev->L;

  if (HEDLEY_LIKELY(ctx->clean)) {
    if (HEDLEY_UNLIKELY(ctx->registry.func == LUA_REFNIL)) {
      return false;
    }
    cp->result = ctx->registry.func;
    cp->ok     = true;
    cp->cb(cp);
    return true;
  }

  cp->lock = (upd_file_lock_t) {
    .file  = f->backend,
    .udata = cp,
    .cb    = compile_lock_bin_cb_,
  };
  upd_file_ref(f);
  if (HEDLEY_UNLIKELY(!upd_file_lock(&cp->lock))) {
    upd_file_unref(f);
    return false;
  }
  ctx->clean = true;
  return true;
}


static bool prog_init_(upd_file_t* f) {
  if (HEDLEY_UNLIKELY(f->backend == NULL)) {
    prog_logf_(f, "requires backend file");
    return false;
  }

  lj_prog_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    prog_logf_(f, "context allocation failure");
    return false;
  }
  *ctx = (lj_prog_t) {
    .watch = {
      .file  = f->backend,
      .udata = f,
      .cb    = prog_watch_bin_cb_,
    },
  };
  f->ctx = ctx;

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    prog_logf_(f, "backend watch failure");
    return false;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .ex    = true,
      .udata = f,
      .cb    = prog_lock_self_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    prog_logf_(f, "init lock failure");
    return false;
  }
  return true;
}

static void prog_deinit_(upd_file_t* f) {
  lj_prog_t*  ctx = f->ctx;
  upd_file_t* dev = ctx->dev;

  upd_file_unwatch(&ctx->watch);

  if (HEDLEY_LIKELY(dev)) {
    luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->registry.func);
    upd_file_unref(dev);
  }

  upd_free(&ctx);
}

static bool prog_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  lj_prog_t*  ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->L == NULL)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  switch (req->type) {
  case UPD_REQ_PROG_EXEC: {
    const bool cp = lj_compile_with_dup(&(lj_compile_t) {
        .prog  = f,
        .udata = req,
        .cb    = prog_compile_cb_,
      });
    ctx->clean = true;
    if (HEDLEY_UNLIKELY(!cp)) {
      req->result = UPD_REQ_INVALID;
      prog_logf_(f, "compile context allocation failure");
      return false;
    }
  } return true;

  default:
    req->file = f->backend;
    return upd_req(req);
  }
}

static upd_file_t* prog_exec_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  lj_prog_t* ctx = f->ctx;

  upd_file_t* stf = upd_file_new(&(upd_file_t) {
      .iso      = iso,
      .driver   = &lj_stream,
      .npath    = f->npath,
      .npathlen = f->npathlen,
      .path     = f->path,
      .pathlen  = f->pathlen,
    });
  if (HEDLEY_UNLIKELY(stf == NULL)) {
    return NULL;
  }

  lj_stream_t* stctx = stf->ctx;
  stctx->dev           = ctx->dev;
  stctx->prog          = f;
  stctx->registry.func = ctx->registry.func;

  upd_file_ref(stctx->dev);
  upd_file_ref(stctx->prog);

  if (HEDLEY_UNLIKELY(!lj_stream_start(stf))) {
    upd_file_unref(stf);
    return NULL;
  }
  return stf;
}

static void prog_logf_(upd_file_t* f, const char* fmt, ...) {
  upd_iso_t* iso = f->iso;

  upd_iso_msgf(iso, LOG_PREFIX_);

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(iso, fmt, args);
  va_end(args);

  upd_iso_msgf(iso, " (%s)\n", f->npath);
}


static void compile_finalize_(lj_compile_t* cp) {
  upd_file_t* f   = cp->prog;
  lj_prog_t*  ctx = f->ctx;

  luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->registry.func);
  ctx->registry.func = cp->ok? cp->result: LUA_REFNIL;

  if (HEDLEY_LIKELY(cp->locked)) {
    upd_file_unlock(&cp->lock);
  }

  cp->cb(cp);
  upd_file_unref(f);
}


static void prog_lock_self_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    prog_logf_(f, "init lock cancelled");
    goto ABORT;
  }

  const bool pf = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso   = iso,
      .path  = (uint8_t*) LJ_DEV_PATH,
      .len   = sizeof(LJ_DEV_PATH)-1,
      .udata = k,
      .cb    = prog_pathfind_dev_cb_,
    });
  if (HEDLEY_UNLIKELY(!pf)) {
    prog_logf_(f, "device pathfind refusal");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void prog_pathfind_dev_cb_(upd_pathfind_t* pf) {
  upd_file_lock_t* k   = pf->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  lj_prog_t*       ctx = f->ctx;

  ctx->dev = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(ctx->dev == NULL)) {
    prog_logf_(f, "no device file found");
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(ctx->dev->driver != &lj_dev)) {
    prog_logf_(f, "device file found, but it's a fake");
    goto EXIT;
  }

  lj_dev_t* devctx = ctx->dev->ctx;
  ctx->L = devctx->L;
  upd_file_ref(ctx->dev);

EXIT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void prog_watch_bin_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->udata;
  lj_prog_t*  ctx = f->ctx;

  if (w->event == UPD_FILE_UPDATE) {
    ctx->clean = false;
    upd_file_trigger(f, UPD_FILE_UPDATE);
  }
}

static void prog_compile_cb_(lj_compile_t* cp) {
  upd_req_t*  req = cp->udata;
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;
  lj_prog_t*  ctx = f->ctx;

  upd_iso_unstack(iso, cp);

  if (HEDLEY_UNLIKELY(ctx->registry.func == LUA_REFNIL)) {
    goto ABORT;
  }

  upd_file_t* stf = prog_exec_(f);
  if (HEDLEY_UNLIKELY(stf == NULL)) {
    prog_logf_(f, "execution failure");
    goto ABORT;
  }

  req->prog.exec = stf;
  req->result    = UPD_REQ_OK;
  req->cb(req);

  upd_file_unref(stf);
  return;

ABORT:
  req->result = UPD_REQ_ABORTED;
  req->cb(req);
}


static void compile_lock_bin_cb_(upd_file_lock_t* k) {
  lj_compile_t* cp = k->udata;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    prog_logf_(cp->prog, "backend lock cancelled");
    goto ABORT;
  }
  cp->locked = true;

  cp->req = (upd_req_t) {
    .file = cp->prog->backend,
    .type = UPD_REQ_STREAM_READ,
    .stream = { .io = {
      .size = SIZE_MAX,
    }, },
    .udata = cp,
    .cb    = compile_read_bin_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&cp->req))) {
    prog_logf_(cp->prog, "backend read refusal");
    goto ABORT;
  }
  return;

ABORT:
  compile_finalize_(cp);
}

static void compile_read_bin_cb_(upd_req_t* req) {
  lj_compile_t* cp = req->udata;
  lua_State*    L  = cp->L;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    prog_logf_(cp->prog, "backend read failure");
    goto EXIT;
  }

  const upd_req_stream_io_t* io = &req->stream.io;
  if (HEDLEY_UNLIKELY(!io->tail)) {
    prog_logf_(cp->prog, "script may be too huge");
    goto EXIT;
  }

  const int ret = luaL_loadbuffer(
    L, (char*) io->buf, io->size, (char*) cp->prog->npath);
  switch (ret) {
  case LUA_ERRSYNTAX:
    prog_logf_(cp->prog, "lua parser syntax error: %s", lua_tostring(L, -1));
    goto EXIT;
  case LUA_ERRMEM:
    prog_logf_(cp->prog,
      "lua parser allocation failure: %s", lua_tostring(L, -1));
    goto EXIT;
  }

  cp->ok     = true;
  cp->result = luaL_ref(L, LUA_REGISTRYINDEX);

EXIT:
  compile_finalize_(cp);
}
