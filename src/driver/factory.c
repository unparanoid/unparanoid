#include "common.h"


#define LOG_PREFIX_ "upd.factory: "


typedef struct ctx_t_ {
  const upd_driver_t* driver;
} ctx_t_;


static
bool
factory_init_(
  upd_file_t* f);

static
void
factory_deinit_(
  upd_file_t* f);

static
void
factory_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_factory = {
  .name = (uint8_t*) "upd.factory",
  .cats = (upd_req_cat_t[]) {
    .UPD_REQ_PROG,
    0,
  },
  .init   = factory_init_,
  .deinit = factory_deinit_,
  .handle = factory_handle_,
};


static bool factory_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  ctx_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    upd_iso_msgf(iso, LOG_PREFIX_"context allocation failure\n");
    return false;
  }
  *ctx = (ctx_t_) {0};

  ctx->driver = upd_driver_lookup(iso, f->param, f->paramlen);
  if (HEDLEY_UNLIKELY(ctx->driver == NULL)) {
    upd_free(&ctx);
    upd_iso_msgf(iso, LOG_PREFIX_"unknown driver '%s'\n", f->param);
    return false;
  }

  f->ctx = ctx;
  return true;
}

static void factory_deinit_(upd_file_t* f) {
  ctx_t_* ctx = f->ctx;
  upd_free(&ctx);
}

static bool factory_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(req->type != UPD_REQ_PROG_EXEC)) {
    req->result = UPD_REQ_INVALID;
    return false;
  }

  req->exec = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = ctx->driver,
    });
  if (HEDLEY_UNLIKELY(req->exec == NULL)) {
    req->result = UPD_REQ_ABORTED;
    upd_iso_msgf(iso, LOG_PREFIX_"product creation failure\n");
    return false;
  }

  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}
