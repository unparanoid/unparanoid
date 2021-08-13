#include "common.h"

#define LOG_PREFIX_ "upd.graphics.gl3.buf: "


static
bool
array_init_(
  upd_file_t* f);

static
bool
element_init_(
  upd_file_t* f);

static
bool
buf_init_(
  upd_file_t* f,
  GLenum      target);

static
void
buf_deinit_(
  upd_file_t* f);

static
bool
buf_handle_(
  upd_req_t* req);

static
bool
buf_handle_meta_(
  upd_req_t* req);

static
bool
buf_handle_data_(
  upd_req_t* req);

static
bool
buf_handle_flush_(
  upd_req_t* req);

const upd_driver_t gra_gl3_buf_array = {
  .name   = (uint8_t*) "upd.graphics.gl3.buf.array",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_TENSOR,
    0,
  },
  .init   = array_init_,
  .deinit = buf_deinit_,
  .handle = buf_handle_,
};

const upd_driver_t gra_gl3_buf_element = {
  .name   = (uint8_t*) "upd.graphics.gl3.buf.element",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_TENSOR,
    0,
  },
  .init   = element_init_,
  .deinit = buf_deinit_,
  .handle = buf_handle_,
};


static
void
buf_init_lock_cb_(
  upd_file_lock_t* k);

static
void
buf_init_pathfind_cb_(
  upd_pathfind_t* pf);

static
void
buf_new_cb_(
  gra_gl3_req_t* req);

static
void
buf_del_cb_(
  gra_gl3_req_t* req);

static
void
buf_map_cb_(
  gra_gl3_req_t* req);

static
void
buf_unmap_cb_(
  gra_gl3_req_t* req);


static bool array_init_(upd_file_t* f) {
  return buf_init_(f, GL_ARRAY_BUFFER);
}

static bool element_init_(upd_file_t* f) {
  return buf_init_(f, GL_ELEMENT_ARRAY_BUFFER);
}

static bool buf_init_(upd_file_t* f, GLenum target) {
  gra_gl3_buf_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (gra_gl3_buf_t) {
    .target = target,
    .broken = true,
  };
  f->ctx = ctx;

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .ex    = true,
      .udata = f,
      .cb    = buf_init_lock_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_free(&ctx);
    return false;
  }
  return true;
}

static void buf_deinit_(upd_file_t* f) {
  upd_iso_t*     iso = f->iso;
  gra_gl3_buf_t* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->gl && ctx->id)) {
    const bool del = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
        .dev  = ctx->gl,
        .type = GRA_GL3_REQ_BUF_DELETE,
        .buf_multi = {
          .n = 1,
          .p = { ctx->id, },
        },
        .udata = iso,
        .cb    = buf_del_cb_,
      });
    if (HEDLEY_UNLIKELY(!del)) {
      upd_iso_msgf(iso, LOG_PREFIX_"GL3_REQ_BUF_DELETE failure\n");
    }
  }
  if (HEDLEY_LIKELY(ctx->gl)) {
    upd_file_unref(ctx->gl);
  }
  upd_free(&ctx);
}

static bool buf_handle_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  gra_gl3_buf_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->gl == NULL || ctx->id == 0)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  switch (req->type) {
  case UPD_REQ_TENSOR_META:
    return buf_handle_meta_(req);

  case UPD_REQ_TENSOR_DATA:
    return buf_handle_data_(req);

  case UPD_REQ_TENSOR_FLUSH:
    return buf_handle_flush_(req);

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static bool buf_handle_meta_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  gra_gl3_buf_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  req->tensor.meta = (upd_req_tensor_meta_t) {
    .type = ctx->type,
    .rank = 1,
    .reso = &ctx->reso,
  };
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}

static bool buf_handle_data_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  gra_gl3_buf_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  ctx->map.mode = GL_READ_ONLY;

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev  = ctx->gl,
      .type = GRA_GL3_REQ_BUF_MAP,
      .buf_map = {
        .id     = ctx->id,
        .target = ctx->target,
        .mode   = ctx->map.mode,
      },
      .udata = req,
      .cb    = buf_map_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }
  return true;
}

static bool buf_handle_flush_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  gra_gl3_buf_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev  = ctx->gl,
      .type = GRA_GL3_REQ_BUF_UNMAP,
      .buf_map = {
        .id     = ctx->id,
        .target = ctx->target,
        .mode   = ctx->map.mode,
      },
      .udata = req,
      .cb    = buf_unmap_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }
  return true;
}


static void buf_init_lock_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"init lock cancelled\n");
    goto ABORT;
  }

  const uint8_t* path    = (void*) GRA_GL3_DEV_PATH;
  size_t         pathlen = sizeof(GRA_GL3_DEV_PATH)-1;
  if (HEDLEY_UNLIKELY(f->param)) {
    path    = f->param;
    pathlen = f->paramlen;
  }

  const bool ok = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso   = iso,
      .path  = path,
      .len   = pathlen,
      .udata = k,
      .cb    = buf_init_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GPU device pathfind failure\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void buf_init_pathfind_cb_(upd_pathfind_t* pf) {
  upd_file_lock_t* k   = pf->udata;
  upd_file_t*      f   = k->udata;
  gra_gl3_buf_t*   ctx = f->ctx;
  upd_iso_t*       iso = f->iso;

  upd_file_t* gl = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(gl == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"no OpenGL3 device found\n");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(gl->driver != &gra_gl3_dev)) {
    upd_iso_msgf(iso, LOG_PREFIX_"OpenGL3 device found but it's fake\n");
    goto ABORT;
  }
  upd_file_ref(gl);
  ctx->gl = gl;

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev   = ctx->gl,
      .type  = GRA_GL3_REQ_BUF_NEW,
      .buf_multi = {
        .n = 1,
      },
      .udata = k,
      .cb    = buf_new_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GL3_REQ_BUF_NEW is refused\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void buf_new_cb_(gra_gl3_req_t* req) {
  upd_file_lock_t* k   = req->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_buf_t*   ctx = f->ctx;

  ctx->id = req->ok? req->buf_multi.p[0]: 0;
  ctx->broken = !ctx->id;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    upd_iso_msgf(iso, LOG_PREFIX_"OpenGL buffer allocation failure\n");
  }

  upd_file_unlock(k);
  upd_iso_unstack(iso, k);

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}

static void buf_del_cb_(gra_gl3_req_t* req) {
  upd_iso_t* iso = req->udata;

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}

static void buf_map_cb_(gra_gl3_req_t* req) {
  upd_req_t*  oreq = req->udata;
  upd_file_t* f    = oreq->file;
  upd_iso_t*  iso  = f->iso;

  oreq->tensor.data.ptr = req->buf_map.data;

  oreq->result = req->ok? UPD_REQ_OK: UPD_REQ_ABORTED;
  oreq->cb(oreq);

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}

static void buf_unmap_cb_(gra_gl3_req_t* req) {
  upd_req_t*  oreq = req->udata;
  upd_file_t* f    = oreq->file;
  upd_iso_t*  iso  = f->iso;

  oreq->result = req->ok? UPD_REQ_OK: UPD_REQ_ABORTED;
  oreq->cb(oreq);

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}
