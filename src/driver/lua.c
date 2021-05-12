#include "common.h"


#define SCRIPT_MAX_ (1024*1024*8)

#define DEV_PATH_ "/sys/upd.dev.lua"

#define SANDBOX_INSTRUCTION_LIMIT_ 10000000  /* = 10^7 (10ms in 1 GHz clock) */


static const char* lua_global_[] = {
  "Iso", "Req",
  "iso",
};


typedef struct prog_t_ {
  upd_file_t* file;
  upd_file_t* dev;

  upd_file_watch_t watch;

  int func_registry_index;

  unsigned clean    : 1;
  unsigned compiled : 1;
} prog_t_;

typedef struct stream_t_ {
  uv_idle_t idle;

  upd_file_t* file;
  upd_file_t* dev;

  lua_State* lua;
  int        lua_registry_index;
  int        env_registry_index;
  int        func_registry_index;

  unsigned exited : 1;
} stream_t_;

typedef struct task_t_ {
  upd_file_t* file;
  upd_req_t*  req;

  uv_file         fd;
  uv_fs_t         fsreq;
  upd_file_lock_t lock;

  size_t   size;
  uint8_t* buf;

  size_t refcnt;
} task_t_;

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

const upd_driver_t upd_driver_lua = {
  .name = (uint8_t*) "upd.lua",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_PROG,
    0,
  },
  .init   = prog_init_,
  .deinit = prog_deinit_,
  .handle = prog_handle_,
};


static
bool
stream_init_(
  upd_file_t* f);

static
void
stream_deinit_(
  upd_file_t* f);

static
bool
stream_handle_(
  upd_req_t* req);

static const upd_driver_t stream_ = {
  .name = (uint8_t*) "upd.lua.stream_",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_STREAM,
    0,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


static
void
lua_logf_(
  upd_file_t* f,
  const char* fmt,
  ...);


static
upd_file_t*
prog_exec_(
  upd_file_t* prog);


static
void
stream_resume_(
  upd_file_t* st);


static
void
task_unref_(
  task_t_* task);


static
void
prog_lock_for_pathfind_cb_(
  upd_file_lock_t* lock);

static
void
prog_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
prog_watch_cb_(
  upd_file_watch_t* w);

static
void
prog_lock_for_delete_cb_(
  upd_file_lock_t* lock);


static
void
stream_lua_hook_cb_(
  lua_State* state,
  lua_Debug* dbg);

static
void
stream_idle_cb_(
  uv_idle_t* idle);

static
void
stream_close_cb_(
  uv_handle_t* handle);


static
void
task_exec_stat_cb_(
  uv_fs_t* fs);

static
void
task_exec_open_cb_(
  uv_fs_t* fs);

static
void
task_exec_read_cb_(
  uv_fs_t* fs);

static
void
task_exec_close_cb_(
  uv_fs_t* fs);

static
void
task_exec_lock_for_loadbuffer_(
  upd_file_lock_t* lock);


static bool prog_init_(upd_file_t* f) {
  prog_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (prog_t_) {
    .file = f,
    .watch = {
      .file = f,
      .cb   = prog_watch_cb_,
    },
  };
  f->ctx = ctx;

  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    return false;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file = f,
      .ex   = true,
      .cb   = prog_lock_for_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    return false;
  }
  return true;
}

static void prog_deinit_(upd_file_t* f) {
  prog_t_*    ctx = f->ctx;
  upd_file_t* dev = ctx->dev;

  bool lock = dev != NULL && ctx->compiled;
  if (HEDLEY_LIKELY(lock)) {
    lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = dev,
        .ex    = true,
        .udata = ctx,
        .cb    = prog_lock_for_delete_cb_,
      });
  }

  if (HEDLEY_LIKELY(dev)) {
    upd_file_unref(dev);
  }
  if (HEDLEY_LIKELY(lock)) {
    return;
  }
  upd_free(&ctx);
}

static bool prog_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  prog_t_*    ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(ctx->dev == NULL)) {
    return false;
  }

  switch (req->type) {
  case UPD_REQ_PROG_ACCESS:
    req->prog.access = (upd_req_prog_access_t) {
      .exec = true,
    };
    break;

  case UPD_REQ_PROG_EXEC: {
    req->prog.exec = NULL;
    if (HEDLEY_LIKELY(ctx->clean && ctx->compiled)) {
      req->prog.exec = prog_exec_(f);
      break;
    }

    task_t_* task = upd_iso_stack(iso, sizeof(*task));
    if (HEDLEY_UNLIKELY(task == NULL)) {
      return false;
    }
    *task = (task_t_) {
      .file   = f,
      .req    = req,
      .fsreq  = { .data = task, },
      .refcnt = 1,
    };
    upd_file_ref(f);

    const bool stat = 0 <= uv_fs_stat(
      &iso->loop, &task->fsreq, (char*) f->npath, task_exec_stat_cb_);
    if (HEDLEY_UNLIKELY(!stat)) {
      task_unref_(task);
      return false;
    }
  } return true;

  default:
    return false;
  }
  req->cb(req);
  return true;
}


static bool stream_init_(upd_file_t* f) {
  stream_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (stream_t_) {
    .file = f,
  };

  if (HEDLEY_UNLIKELY(0 > uv_idle_init(&f->iso->loop, &ctx->idle))) {
    upd_free(&ctx);
    return false;
  }
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  stream_t_* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->dev)) {
    upd_file_unref(ctx->dev);
  }
  if (HEDLEY_LIKELY(ctx->lua)) {
    luaL_unref(ctx->lua, LUA_REGISTRYINDEX, ctx->lua_registry_index);
  }
  uv_close((uv_handle_t*) &ctx->idle, stream_close_cb_);
}

static bool stream_handle_(upd_req_t* req) {
  (void) req;
  return false;
}


static void lua_logf_(upd_file_t* f, const char* fmt, ...) {
  upd_iso_t* iso = f->iso;

  upd_iso_msgf(iso, "upd.lua error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(iso, fmt, args);
  va_end(args);

  upd_iso_msgf(iso, " (%s)\n", f->npath);
}


static upd_file_t* prog_exec_(upd_file_t* prog) {
  prog_t_*    ctx = prog->ctx;
  upd_iso_t*  iso = prog->iso;
  upd_file_t* dev = ctx->dev;
  lua_State*  lua = dev->ctx;

  upd_file_t* f = upd_file_new(iso, &stream_);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    return NULL;
  }

  stream_t_* st = f->ctx;
  st->dev = dev;

  upd_file_ref(dev);

  lua_State* th = lua_newthread(lua);
  if (HEDLEY_UNLIKELY(th == NULL)) {
    upd_file_unref(f);
    return NULL;
  }

  st->lua = th;
  st->lua_registry_index = luaL_ref(lua, LUA_REGISTRYINDEX);

  lua_sethook(th,
    stream_lua_hook_cb_, LUA_MASKCOUNT, SANDBOX_INSTRUCTION_LIMIT_);

  lua_createtable(th, 0, 0);
  for (size_t i = 0; i < sizeof(lua_global_)/sizeof(lua_global_[0]); ++i) {
    lua_getglobal(th, lua_global_[i]);
    lua_setfield(th, -2, lua_global_[i]);
  }
  st->env_registry_index = luaL_ref(th, LUA_REGISTRYINDEX);

  st->func_registry_index = ctx->func_registry_index;
  lua_rawgeti(th, LUA_REGISTRYINDEX, st->func_registry_index);

  stream_resume_(f);
  return f;
}


static void stream_exit_(upd_file_t* st) {
  stream_t_* ctx = st->ctx;

  ctx->exited = true;
  upd_file_unref(st);
}

static void stream_resume_(upd_file_t* st) {
  stream_t_* ctx = st->ctx;
  lua_State* lua = ctx->lua;

  lua_rawgeti(lua, LUA_REGISTRYINDEX, ctx->func_registry_index);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, ctx->env_registry_index);
  lua_setfenv(lua, -2);
  lua_pop(lua, 1);

  const int err = lua_resume(lua, 0);
  /* TODO: pop stack */

  switch (err) {
  case LUA_OK:
    ctx->exited = true;
    stream_exit_(st);
    break;
  case LUA_YIELD:
    if (HEDLEY_UNLIKELY(!uv_is_active((uv_handle_t*) &ctx->idle))) {
      if (HEDLEY_UNLIKELY(0 > uv_idle_start(&ctx->idle, stream_idle_cb_))) {
        lua_logf_(st, "failed to start runner");
        stream_exit_(st);
      }
    }
    break;
  case LUA_ERRRUN:
  case LUA_ERRMEM:
  case LUA_ERRERR: {
    lua_Debug dbg;
    lua_getstack(lua, 0, &dbg);
    lua_getinfo(lua, "Sl", &dbg);
    lua_logf_(st,
      "runtime error (%s) (%s:%d)",
      lua_tostring(lua, -1), dbg.source, dbg.currentline);
    stream_exit_(st);
  } break;
  }
}


static void task_unref_(task_t_* task) {
  upd_file_t* f   = task->file;
  upd_iso_t*  iso = f->iso;
  upd_req_t*  req = task->req;

  assert(task->refcnt);
  if (HEDLEY_UNLIKELY(--task->refcnt == 0)) {
    if (req) {
      req->cb(req);
    }
    upd_file_unref(f);
    upd_iso_unstack(iso, task);
  }
}


static void prog_lock_for_pathfind_cb_(upd_file_lock_t* lock) {
  upd_file_t* f   = lock->file;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto ABORT;
  }

  const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso   = f->iso,
      .path  = (uint8_t*) DEV_PATH_,
      .len   = utf8size_lazy(DEV_PATH_),
      .udata = lock,
      .cb    = prog_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!pf)) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
}

static void prog_pathfind_cb_(upd_req_pathfind_t* pf) {
  upd_file_lock_t* lock = pf->udata;
  upd_file_t*      f    = lock->file;
  prog_t_*         ctx  = f->ctx;
  upd_iso_t*       iso  = f->iso;

  upd_file_t* dev = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(dev == NULL)) {
    lua_logf_(f, "'"DEV_PATH_"' is not found");
    goto EXIT;
  }

  if (HEDLEY_UNLIKELY(dev->driver != &upd_driver_dev_lua)) {
    lua_logf_(f, "'"DEV_PATH_"' is not lua_State device");
    goto EXIT;
  }

  upd_file_ref(dev);
  ctx->dev = dev;

EXIT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
}

static void prog_watch_cb_(upd_file_watch_t* w) {
  upd_file_t* f   = w->file;
  prog_t_*    ctx = f->ctx;

  if (HEDLEY_UNLIKELY(w->event == UPD_FILE_UPDATE_N)) {
    ctx->clean = false;
  }
}

static void prog_lock_for_delete_cb_(upd_file_lock_t* lock) {
  prog_t_*    ctx = lock->udata;
  upd_file_t* dev = lock->file;
  upd_iso_t*  iso = dev->iso;
  lua_State*  lua = dev->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto EXIT;
  }
  luaL_unref(lua, LUA_REGISTRYINDEX, ctx->func_registry_index);

EXIT:
  upd_free(&ctx);
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
}


static void stream_lua_destroy_hook_cb_(lua_State* lua, lua_Debug* dbg) {
  (void) dbg;
  luaL_error(lua, "reaches instruction limit %d", SANDBOX_INSTRUCTION_LIMIT_);
}
static void stream_lua_hook_cb_(lua_State* lua, lua_Debug* dbg) {
  (void) dbg;
  lua_sethook(lua, stream_lua_destroy_hook_cb_, LUA_MASKLINE, 0);
}

static void stream_idle_cb_(uv_idle_t* idle) {
  stream_t_*  ctx = (void*) idle;
  upd_file_t* f   = ctx->file;

  stream_resume_(f);

  if (HEDLEY_UNLIKELY(ctx->exited)) {
    uv_idle_stop(&ctx->idle);
  }
}

static void stream_close_cb_(uv_handle_t* handle) {
  stream_t_* ctx = (void*) handle;
  upd_free(&ctx);
}


static void task_exec_stat_cb_(uv_fs_t* fsreq) {
  task_t_*    task = fsreq->data;
  upd_file_t* f    = task->file;
  upd_iso_t*  iso  = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result != 0)) {
    lua_logf_(f, "stat failure");
    goto ABORT;
  }

  task->size = fsreq->statbuf.st_size;
  if (HEDLEY_UNLIKELY(task->size > SCRIPT_MAX_)) {
    lua_logf_(f, "too huge script (%zu exceeds %zu)", task->size, SCRIPT_MAX_);
    goto ABORT;
  }

  const bool open = 0 <= uv_fs_open(
    &iso->loop, fsreq, (char*) f->npath, 0, O_RDONLY, task_exec_open_cb_);
  if (HEDLEY_UNLIKELY(!open)) {
    lua_logf_(f, "open failure");
    goto ABORT;
  }
  return;

ABORT:
  task_unref_(task);
}

static void task_exec_open_cb_(uv_fs_t* fsreq) {
  task_t_*    task = fsreq->data;
  upd_file_t* f    = task->file;
  upd_iso_t*  iso  = f->iso;

  task->fd = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(task->fd < 0)) {
    lua_logf_(f, "open failure");
    goto ABORT;
  }

  task->buf = upd_iso_stack(iso, task->size);
  if (HEDLEY_UNLIKELY(task->buf == NULL)) {
    lua_logf_(f, "buffer allocation failure");
    goto CLOSE;
  }

  uv_buf_t uvbuf = uv_buf_init((char*) task->buf, task->size);

  const bool read = 0 <= uv_fs_read(
    &iso->loop, fsreq, task->fd, &uvbuf, 1, 0, task_exec_read_cb_);
  if (HEDLEY_UNLIKELY(!read)) {
    upd_iso_unstack(iso, task->buf);
    lua_logf_(f, "read failure");
    goto CLOSE;
  }
  return;

  bool close;
CLOSE:
  close = 0 <= uv_fs_close(&iso->loop, fsreq, task->fd, task_exec_close_cb_);
  if (HEDLEY_UNLIKELY(!close)) {
    goto ABORT;
  }
  return;

ABORT:
  task_unref_(task);
}

static void task_exec_read_cb_(uv_fs_t* fsreq) {
  task_t_*    task = fsreq->data;
  upd_file_t* f    = task->file;
  prog_t_*    ctx  = f->ctx;
  upd_iso_t*  iso  = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    upd_iso_unstack(iso, task->buf);
    lua_logf_(f, "read failure");
    goto EXIT;
  }

  task->lock = (upd_file_lock_t) {
    .file  = ctx->dev,
    .ex    = true,
    .udata = task,
    .cb    = task_exec_lock_for_loadbuffer_,
  };
  ++task->refcnt;
  if (HEDLEY_UNLIKELY(!upd_file_lock(&task->lock))) {
    task_unref_(task);
    upd_iso_unstack(iso, task->buf);
    lua_logf_(f, "lock allocation failure");
    goto EXIT;
  }

  bool close;
EXIT:
  close = 0 <= uv_fs_close(&iso->loop, fsreq, task->fd, task_exec_close_cb_);
  if (HEDLEY_UNLIKELY(!close)) {
    task_unref_(task);
    return;
  }
}

static void task_exec_close_cb_(uv_fs_t* fsreq) {
  task_t_* task = fsreq->data;

  uv_fs_req_cleanup(fsreq);
  task_unref_(task);
}

static void task_exec_lock_for_loadbuffer_(upd_file_lock_t* lock) {
  task_t_*    task = lock->udata;
  upd_file_t* f    = task->file;
  upd_req_t*  req  = task->req;
  prog_t_*    ctx  = f->ctx;
  lua_State*  lua  = ctx->dev->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    goto EXIT;
  }

  if (HEDLEY_LIKELY(ctx->compiled)) {
    luaL_unref(lua, LUA_REGISTRYINDEX, ctx->func_registry_index);
    ctx->clean    = false;
    ctx->compiled = false;
  }

  const int load = luaL_loadbuffer(
    lua, (char*) task->buf, task->size, (char*) f->npath);

  switch (load) {
  case LUA_ERRSYNTAX:
    lua_logf_(f, "lua parser syntax error");
    goto EXIT;
  case LUA_ERRMEM:
    lua_logf_(f, "lua parser allocation failure");
    goto EXIT;
  }

  ctx->clean    = true;
  ctx->compiled = true;

  ctx->func_registry_index = luaL_ref(lua, LUA_REGISTRYINDEX);

  req->prog.exec = prog_exec_(f);

EXIT:
  upd_file_unlock(lock);
  task_unref_(task);
}
