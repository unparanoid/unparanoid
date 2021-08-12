#include "common.h"

#define LOG_PREFIX_ "upd.graphics.gl3.tex: "


static
bool
tex1d_init_(
  upd_file_t* f);

static
bool
tex2d_init_(
  upd_file_t* f);

static
bool
tex3d_init_(
  upd_file_t* f);

static
bool
tex_init_(
  upd_file_t* f,
  uint8_t     rank,
  GLenum      target);

static
void
tex_deinit_(
  upd_file_t* f);

static
bool
tex_handle_(
  upd_req_t* req);

static
bool
tex_handle_alloc_(
  upd_req_t* req);

static
bool
tex_handle_meta_(
  upd_req_t* req);

static
bool
tex_handle_data_(
  upd_req_t* req);

static
bool
tex_handle_flush_(
  upd_req_t* req);

const upd_driver_t gra_gl3_tex_1d = {
  .name   = (uint8_t*) "upd.graphics.gl3.tex.1d",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_TENSOR,
    0,
  },
  .init   = tex1d_init_,
  .deinit = tex_deinit_,
  .handle = tex_handle_,
};

const upd_driver_t gra_gl3_tex_2d = {
  .name   = (uint8_t*) "upd.graphics.gl3.tex.2d",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_TENSOR,
    0,
  },
  .init   = tex2d_init_,
  .deinit = tex_deinit_,
  .handle = tex_handle_,
};

const upd_driver_t gra_gl3_tex_3d = {
  .name   = (uint8_t*) "upd.graphics.gl3.tex.3d",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_TENSOR,
    0,
  },
  .init   = tex3d_init_,
  .deinit = tex_deinit_,
  .handle = tex_handle_,
};


static
void
tex_lock_for_init_cb_(
  upd_file_lock_t* k);

static
void
tex_pathfind_dev_cb_(
  upd_pathfind_t* pf);

static
void
tex_new_cb_(
  gra_gl3_req_t* req);

static
void
tex_buf_new_cb_(
  gra_gl3_req_t* req);

static
void
tex_del_cb_(
  gra_gl3_req_t* req);

static
void
tex_buf_del_cb_(
  gra_gl3_req_t* req);

static
void
tex_alloc_cb_(
  gra_gl3_req_t* req);

static
void
tex_buf_map_pbo_cb_(
  gra_gl3_req_t* req);

static
void
tex_buf_unmap_pbo_cb_(
  gra_gl3_req_t* req);


static bool tex1d_init_(upd_file_t* f) {
  return tex_init_(f, 2, GL_TEXTURE_1D);
}

static bool tex2d_init_(upd_file_t* f) {
  return tex_init_(f, 3, GL_TEXTURE_2D);
}

static bool tex3d_init_(upd_file_t* f) {
  return tex_init_(f, 4, GL_TEXTURE_3D);
}

static bool tex_init_(upd_file_t* f, uint8_t rank, GLenum target) {
  gra_gl3_tex_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (gra_gl3_tex_t) {
    .rank   = rank,
    .target = target,
    .broken = true,
  };
  f->ctx = ctx;
  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .ex    = true,
      .udata = f,
      .cb    = tex_lock_for_init_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_free(&ctx);
    return false;
  }
  return true;
}

static void tex_deinit_(upd_file_t* f) {
  upd_iso_t*     iso = f->iso;
  gra_gl3_tex_t* ctx = f->ctx;

  if (HEDLEY_LIKELY(ctx->gl)) {
    if (HEDLEY_LIKELY(ctx->id)) {
      const bool texdel = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
          .dev  = ctx->gl,
          .type = GRA_GL3_REQ_TEX_DELETE,
          .tex  = {
            .id = ctx->id,
          },
          .udata = iso,
          .cb    = tex_del_cb_,
        });
      if (HEDLEY_UNLIKELY(!texdel)) {
        upd_iso_msgf(iso, LOG_PREFIX_"GL3_REQ_TEX_DELETE failure\n");
      }
    }
    if (HEDLEY_LIKELY(ctx->pbo)) {
      const bool bufdel = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
          .dev  = ctx->gl,
          .type = GRA_GL3_REQ_BUF_DELETE,
          .buf_multi = {
            .n = 1,
            .p = { ctx->pbo, },
          },
          .udata = iso,
          .cb    = tex_buf_del_cb_,
        });
      if (HEDLEY_UNLIKELY(!bufdel)) {
        upd_iso_msgf(iso, LOG_PREFIX_"GL3_REQ_BUF_DELETE failure\n");
      }
    }
    upd_file_unref(ctx->gl);
  }
  upd_free(&ctx);
}

static bool tex_handle_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  gra_gl3_tex_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!ctx->gl || !ctx->id || !ctx->pbo)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  switch (req->type) {
  case UPD_REQ_TENSOR_ALLOC:
    return tex_handle_alloc_(req);

  case UPD_REQ_TENSOR_META:
    return tex_handle_meta_(req);

  case UPD_REQ_TENSOR_DATA:
    return tex_handle_data_(req);

  case UPD_REQ_TENSOR_FLUSH:
    return tex_handle_flush_(req);

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static bool tex_handle_alloc_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  gra_gl3_tex_t* ctx = f->ctx;

  const upd_req_tensor_meta_t* m = &req->tensor.meta;

  const GLenum target =
    m->rank == 2? GL_TEXTURE_1D:
    m->rank == 3? GL_TEXTURE_2D:
    m->rank == 4? GL_TEXTURE_3D: 0;

  const GLenum type =
    m->type == UPD_TENSOR_U8?  GL_UNSIGNED_BYTE:
    m->type == UPD_TENSOR_U16? GL_UNSIGNED_SHORT:
    m->type == UPD_TENSOR_F32? GL_FLOAT: 0;

  if (HEDLEY_UNLIKELY(target != ctx->target || type == 0)) {
    req->result = UPD_REQ_INVALID;
    req->cb(req);
    return false;
  }

  const GLenum fmt =
    m->reso[0] == 1? GL_RED:
    m->reso[0] == 2? GL_RG:
    m->reso[0] == 3? GL_RGB:
    m->reso[0] == 4? GL_RGBA: 0;
  if (HEDLEY_UNLIKELY(fmt == 0)) {
    req->result = UPD_REQ_INVALID;
    req->cb(req);
    return false;
  }

  ctx->broken = true;
  ctx->fmt    = fmt;

  switch (m->rank) {
  case 4:
    ctx->d = m->reso[3];
    HEDLEY_FALL_THROUGH;
  case 3:
    ctx->h = m->reso[2];
    HEDLEY_FALL_THROUGH;
  case 2:
    ctx->w  = m->reso[1];
    ctx->ch = m->reso[0];
  }

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev = ctx->gl,
      .type = GRA_GL3_REQ_TEX_ALLOC,
      .tex = {
        .target = target,
        .id     = ctx->id,
        .fmt    = fmt,
        .type   = type,
        .w      = ctx->w,
        .h      = ctx->h,
        .d      = ctx->d,
        .data   = req->tensor.data.ptr,
      },
      .udata = req,
      .cb    = tex_alloc_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    req->result = UPD_REQ_ABORTED;
    req->cb(req);
    return false;
  }
  return true;
}

static bool tex_handle_meta_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  gra_gl3_tex_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  const uint32_t reso[4] = {
    ctx->ch, ctx->w, ctx->h, ctx->d,
  };
  req->tensor.meta = (upd_req_tensor_meta_t) {
    .rank    = ctx->rank,
    .type    = UPD_TENSOR_F32,
    .reso    = (uint32_t*) reso,
    .inplace = true,
  };
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}

static bool tex_handle_data_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  gra_gl3_tex_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  const bool inplace = req->tensor.meta.inplace;
  ctx->map.mode = inplace? GL_READ_WRITE: GL_READ_ONLY;

  const upd_tensor_type_t type = req->tensor.meta.type;
  ctx->map.type =
    type == UPD_TENSOR_U8?  GL_UNSIGNED_BYTE:
    type == UPD_TENSOR_U16? GL_UNSIGNED_SHORT:
    type == UPD_TENSOR_F32? GL_FLOAT: 0;

  const size_t size =  /* WIP */
    ctx->ch * ctx->w * ctx->h * 8;

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev   = ctx->gl,
      .type  = GRA_GL3_REQ_BUF_MAP_PBO,
      .buf_map_pbo = {
        .id         = ctx->pbo,
        .mode       = ctx->map.mode,
        .tex        = ctx->id,
        .tex_target = ctx->target,
        .fmt        = ctx->fmt,
        .type       = ctx->map.type,
        .size       = size,
      },
      .udata = req,
      .cb    = tex_buf_map_pbo_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }
  return true;
}

static bool tex_handle_flush_(upd_req_t* req) {
  upd_file_t*    f   = req->file;
  upd_iso_t*     iso = f->iso;
  gra_gl3_tex_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  const bool unmap = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev  = ctx->gl,
      .type = GRA_GL3_REQ_BUF_UNMAP_PBO,
      .buf_map_pbo = {
        .id         = ctx->pbo,
        .mode       = ctx->map.mode,
        .tex        = ctx->id,
        .tex_target = ctx->target,
        .fmt        = ctx->fmt,
        .type       = ctx->map.type,
        .w          = ctx->w,
        .h          = ctx->h,
        .d          = ctx->d,
      },
      .udata = req,
      .cb    = tex_buf_unmap_pbo_cb_,
    });
  if (HEDLEY_UNLIKELY(!unmap)) {
    upd_iso_msgf(iso, LOG_PREFIX_"mapped buffer may be leaked X(\n");
    req->result = UPD_REQ_ABORTED;
    return false;
  }
  return true;
}


static void tex_lock_for_init_cb_(upd_file_lock_t* k) {
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
      .cb    = tex_pathfind_dev_cb_,
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

static void tex_pathfind_dev_cb_(upd_pathfind_t* pf) {
  upd_file_lock_t* k   = pf->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_tex_t*   ctx = f->ctx;

  upd_file_t* gl = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(gl == NULL)) {
    upd_iso_msgf(iso, LOG_PREFIX_"no OpenGL device found\n");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(gl->driver != &gra_gl3_dev)) {
    upd_iso_msgf(iso, LOG_PREFIX_"OpenGL device found, but it's fake\n");
    goto ABORT;
  }

  upd_file_ref(gl);
  ctx->gl = gl;

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev   = ctx->gl,
      .type  = GRA_GL3_REQ_TEX_NEW,
      .udata = k,
      .cb    = tex_new_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GL3_REQ_TEX_NEW failure\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void tex_new_cb_(gra_gl3_req_t* req) {
  upd_file_lock_t* k   = req->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_tex_t*   ctx = f->ctx;

  ctx->id     = req->ok? req->tex.id: 0;
  ctx->broken = !ctx->id;

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    upd_iso_msgf(iso, LOG_PREFIX_"texture allocation failure\n");
    goto ABORT;
  }

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev   = ctx->gl,
      .type  = GRA_GL3_REQ_BUF_NEW,
      .buf_multi = { .n = 1, },
      .udata = k,
      .cb    = tex_buf_new_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, LOG_PREFIX_"GL3_REQ_BUF_NEW failure\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void tex_buf_new_cb_(gra_gl3_req_t* req) {
  upd_file_lock_t* k   = req->udata;
  upd_file_t*      f   = k->udata;
  upd_iso_t*       iso = f->iso;
  gra_gl3_tex_t*   ctx = f->ctx;

  ctx->pbo    = req->ok? req->buf_multi.p[0]: 0;
  ctx->broken = !ctx->pbo;

  if (HEDLEY_UNLIKELY(ctx->broken)) {
    upd_iso_msgf(iso, LOG_PREFIX_"PBO allocation failure\n");
  }

  upd_file_unlock(k);
  upd_iso_unstack(iso, k);

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}

static void tex_del_cb_(gra_gl3_req_t* req) {
  upd_iso_t* iso = req->udata;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}

static void tex_buf_del_cb_(gra_gl3_req_t* req) {
  upd_iso_t* iso = req->udata;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}

static void tex_alloc_cb_(gra_gl3_req_t* req) {
  upd_req_t*     oreq = req->udata;
  upd_file_t*    f    = oreq->file;
  upd_iso_t*     iso  = f->iso;
  gra_gl3_tex_t* ctx  = f->ctx;

  ctx->broken = !req->ok;

  oreq->result = req->ok? UPD_REQ_OK: UPD_REQ_ABORTED;
  oreq->cb(oreq);

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}

static void tex_buf_map_pbo_cb_(gra_gl3_req_t* req) {
  upd_req_t*     oreq = req->udata;
  upd_file_t*    f    = oreq->file;
  upd_iso_t*     iso  = f->iso;
  gra_gl3_tex_t* ctx  = f->ctx;

  const upd_tensor_type_t type    = oreq->tensor.meta.type;
  const bool              inplace = oreq->tensor.meta.inplace;

  oreq->tensor.data = (upd_req_tensor_data_t) {
    .meta = {
      .type    = type,
      .rank    = ctx->rank,
      .reso    = (uint32_t[]) { ctx->ch, ctx->w, ctx->h, ctx->d, },
      .inplace = inplace,
    },
    .ptr  = req->buf_map_pbo.data,
    .size = req->buf_map_pbo.size,
  };

  oreq->result = req->ok? UPD_REQ_OK: UPD_REQ_ABORTED;
  oreq->cb(oreq);

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}

static void tex_buf_unmap_pbo_cb_(gra_gl3_req_t* req) {
  upd_req_t*  oreq = req->udata;
  upd_file_t* f    = oreq->file;
  upd_iso_t*  iso  = f->iso;

  oreq->result = req->ok? UPD_REQ_OK: UPD_REQ_ABORTED;
  oreq->cb(oreq);

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}
