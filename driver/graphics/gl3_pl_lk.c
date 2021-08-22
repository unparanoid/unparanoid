#include "common.h"

/*
 *  This stream doesn't satisfy the effect interface.
 * So wrap it with an external one like Lua script.
 */

#define LOG_PREFIX_ "upd.graphics.gl3.pipeline.linker: "


typedef struct ctx_t_      ctx_t_;
typedef struct external_t_ external_t_;
typedef struct copy_t_     copy_t_;
typedef struct assign_t_   assign_t_;

struct ctx_t_ {
  /* No needs to unref, because this file is locked while entire of lifetime. */
  upd_file_t*     def;
  upd_file_lock_t lock;

  upd_msgpack_t    mpk;
  msgpack_unpacked upkd;

  const gra_gl3_pl_t* pl;
  uint8_t*            varbuf;

  size_t refcnt;

  upd_array_of(external_t_*) external;

  upd_array_of(upd_file_t*) in;
  upd_array_of(upd_file_t*) out;

  upd_array_of(upd_file_lock_t*) reslock;

  unsigned locked  : 1;
  unsigned linked  : 1;
  unsigned aborted : 1;
};


static
bool
lk_init_(
  upd_file_t* f);

static
void
lk_deinit_(
  upd_file_t* f);

static
void
lk_teardown_(
  ctx_t_* ctx);

static
bool
lk_handle_(
  upd_req_t* req);

static
void
lk_clear_(
  upd_file_t* f);

static
void
lk_handle_link_(
  upd_file_t* f);

static
void
lk_unref_link_(
  upd_file_t* f);

static
void
lk_handle_exec_(
  upd_file_t* f);

static
void
lk_unref_exec_(
  upd_file_t* f);

static
void
lk_error_(
  upd_file_t* f,
  const char* msg);

static
bool
lk_assign_input_(
  upd_file_t*             f,
  const gra_gl3_pl_var_t* var,
  const msgpack_object*   obj);

static
bool
lk_assign_input_file_(
  upd_file_t*             f,
  const gra_gl3_pl_var_t* var,
  upd_file_t*             gl);

static
bool
lk_create_and_assign_output_tex_(
  upd_file_t*             f,
  const gra_gl3_pl_out_t* out);

static
void
lk_msgpack_cb_(
  upd_msgpack_t* mpk);

static
void
lk_link_cb_(
  gra_gl3_req_t* req);

static
void
lk_lock_for_exec_cb_(
  upd_file_lock_t* k);

static
void
lk_exec_cb_(
  gra_gl3_req_t* req);

static
void
lk_unlink_cb_(
  gra_gl3_req_t* req);

static
void
lk_lock_pl_cb_(
  upd_file_lock_t* k);

static
void
lk_fetch_pl_cb_(
  gra_gl3_fetch_t* fe);

const upd_driver_t gra_gl3_pl_lk = {
  .name   = (uint8_t*) "upd.graphics.gl3.pipeline.linker",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_DSTREAM,
    0,
  },
  .init   = lk_init_,
  .deinit = lk_deinit_,
  .handle = lk_handle_,
};


struct external_t_ {
  upd_file_t* lk;

  const gra_gl3_pl_var_t* var;

  upd_file_t* src;
  upd_file_t* dst;
};


struct assign_t_ {
  upd_file_t* lk;

  const gra_gl3_pl_var_t* var;

  upd_file_t* file;  /* tex or buf */

  upd_file_lock_t lock;
  upd_req_t       req;
  gra_gl3_req_t   greq;
  uint32_t        reso[4];
};

static
void
assign_lock_input_cb_(
  upd_file_lock_t* k);

static
void
assign_lock_output_cb_(
  upd_file_lock_t* k);

static
void
assign_alloc_output_cb_(
  gra_gl3_req_t* greq);


struct copy_t_ {
  external_t_* ext;

  upd_file_lock_t src_lock;
  upd_file_lock_t dst_lock;
  upd_req_t       req;
  gra_gl3_req_t   greq;

  unsigned src_locked  : 1;
  unsigned src_fetched : 1;
  unsigned dst_locked  : 1;
};

static
void
copy_finalize_(
  copy_t_* cpy);

static
void
copy_lock_dst_cb_(
  upd_file_lock_t* k);

static
void
copy_lock_src_cb_(
  upd_file_lock_t* k);

static
void
copy_fetch_src_cb_(
  upd_req_t* req);

static
void
copy_alloc_dst_cb_(
  gra_gl3_req_t* req);

static
void
copy_flush_src_cb_(
  upd_req_t* req);


static bool lk_init_(upd_file_t* f) {
  ctx_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (ctx_t_) {0};

  if (HEDLEY_UNLIKELY(!upd_msgpack_init(&ctx->mpk))) {
    upd_free(&ctx);
    return false;
  }
  ctx->mpk.udata = f;
  ctx->mpk.cb    = lk_msgpack_cb_;

  f->ctx = ctx;
  return true;
}

static void lk_deinit_(upd_file_t* f) {
  ctx_t_* ctx = f->ctx;

  upd_msgpack_deinit(&ctx->mpk);

  lk_clear_(f);

  if (HEDLEY_LIKELY(ctx->locked)) {
    gra_gl3_pl_def_t* def = ctx->def->ctx;
    const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
        .dev   = def->gl,
        .type  = GRA_GL3_REQ_PL_UNLINK,
        .pl    = {
          .ptr    = ctx->pl,
          .varbuf = ctx->varbuf,
        },
        .udata = ctx,
        .cb    = lk_unlink_cb_,
      });
    if (HEDLEY_LIKELY(ok)) {
      return;
    }
  }
  lk_teardown_(ctx);
}

static void lk_teardown_(ctx_t_* ctx) {
  if (HEDLEY_LIKELY(ctx->locked)) {
    upd_file_unlock(&ctx->lock);
  }
  upd_free(&ctx->varbuf);
  upd_free(&ctx);
}

static bool lk_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(ctx->mpk.broken)) {
    req->result = UPD_REQ_ABORTED;
    return false;
  }

  switch (req->type) {
  case UPD_REQ_DSTREAM_WRITE:
  case UPD_REQ_DSTREAM_READ:
    return upd_msgpack_handle(&ctx->mpk, req);

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static void lk_clear_(upd_file_t* f) {
  ctx_t_* ctx = f->ctx;

  for (size_t i = 0; i < ctx->in.n; ++i) {
    upd_file_unref(ctx->in.p[i]);
  }
  upd_array_clear(&ctx->in);

  for (size_t i = 0; i < ctx->out.n; ++i) {
    upd_file_unref(ctx->out.p[i]);
  }
  upd_array_clear(&ctx->out);

  for (size_t i = 0; i < ctx->external.n; ++i) {
    external_t_* ext = ctx->external.p[i];
    upd_file_unref(ext->src);
    upd_file_unref(ext->dst);
    upd_free(&ext);
  }
  upd_array_clear(&ctx->external);
}

static void lk_handle_link_(upd_file_t* f) {
  ctx_t_*    ctx = f->ctx;
  upd_iso_t* iso = f->iso;

  ctx->aborted = false;

  if (HEDLEY_UNLIKELY(ctx->upkd.data.type != MSGPACK_OBJECT_MAP)) {
    lk_error_(f, "object root must be a map");
    return;
  }

  uint64_t target = 0;
  const char* invalid =
    upd_msgpack_find_fields(&ctx->upkd.data.via.map, (upd_msgpack_field_t[]) {
        { .name = "target", .ui = &target, },
        { NULL },
      });
  if (HEDLEY_UNLIKELY(invalid)) {
    lk_error_(f, "invalid req for linking");
    return;
  }

  upd_file_t* def = upd_file_get(iso, target);
  if (HEDLEY_UNLIKELY(def == NULL || def->driver != &gra_gl3_pl_def)) {
    lk_error_(f, "invalid target");
    return;
  }
  ctx->def = def;

  ctx->lock = (upd_file_lock_t) {
    .file  = def,
      .udata = f,
      .cb    = lk_lock_pl_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&ctx->lock))) {
    lk_error_(f, "target lock refusal");
    return;
  }
}

static void lk_unref_link_(upd_file_t* f) {
  ctx_t_* ctx = f->ctx;
  assert(ctx->refcnt);

  if (HEDLEY_LIKELY(--ctx->refcnt)) {
    return;
  }

  if (HEDLEY_UNLIKELY(ctx->aborted)) {
    lk_error_(f, "link proprocess failure");
    return;
  }

  gra_gl3_pl_def_t* def = ctx->def->ctx;
  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev  = def->gl,
      .type = GRA_GL3_REQ_PL_LINK,
      .pl   = {
      .ptr    = ctx->pl,
      .varbuf = ctx->varbuf,
      },
      .udata = f,
      .cb    = lk_link_cb_,
      });
  if (HEDLEY_UNLIKELY(!ok)) {
    lk_error_(f, "pipeline link refusal");
    return;
  }
}

static void lk_handle_exec_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  ctx_t_*    ctx = f->ctx;

  ctx->aborted = false;
  ctx->refcnt  = 1;

  for (size_t i = 0; i < ctx->external.n; ++i) {
    external_t_* ext = ctx->external.p[i];

    copy_t_* cpy = upd_iso_stack(iso, sizeof(*cpy));
    if (HEDLEY_UNLIKELY(cpy == NULL)) {
      ctx->aborted = true;
      goto EXIT;
    }
    *cpy = (copy_t_) {
      .ext = ext,
      .dst_lock = (upd_file_lock_t) {
        .file  = ext->dst,
        .ex    = true,
        .udata = cpy,
        .cb    = copy_lock_dst_cb_,
      },
    };
    ++ctx->refcnt;
    if (HEDLEY_UNLIKELY(!upd_file_lock(&cpy->dst_lock))) {
      --ctx->refcnt;
      ctx->aborted = true;
      goto EXIT;
    }
  }

  for (size_t i = 0; i < ctx->in.n; ++i) {
    ++ctx->refcnt;
    upd_file_lock_t* k = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = ctx->in.p[i],
        .udata = f,
        .cb    = lk_lock_for_exec_cb_,
        });
    if (HEDLEY_UNLIKELY(k == NULL)) {
      --ctx->refcnt;
      ctx->aborted = true;
      goto EXIT;
    }
  }
  for (size_t i = 0; i < ctx->out.n; ++i) {
    upd_file_lock_t* k = upd_file_lock_with_dup(&(upd_file_lock_t) {
        .file  = ctx->out.p[i],
        .ex    = true,
        .udata = f,
        .cb    = lk_lock_for_exec_cb_,
        });
    if (HEDLEY_UNLIKELY(k == NULL)) {
      --ctx->refcnt;
      ctx->aborted = true;
      goto EXIT;
    }
  }

EXIT:
  lk_unref_exec_(f);
}

static void lk_unref_exec_(upd_file_t* f) {
  ctx_t_*             ctx = f->ctx;
  const gra_gl3_pl_t* pl  = ctx->pl;

  if (HEDLEY_LIKELY(--ctx->refcnt)) {
    return;
  }

  if (HEDLEY_UNLIKELY(ctx->aborted)) {
    lk_error_(f, "preprocess failure");
    return;
  }

  /* TODO: check buffer size */

  const gra_gl3_pl_def_t* def = ctx->def->ctx;
  upd_file_t*             gl  = def->gl;

  const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
      .dev   = gl,
      .type  = GRA_GL3_REQ_PL_EXEC,
      .pl    = {
      .ptr    = pl,
        .varbuf = ctx->varbuf,
      },
      .udata = f,
      .cb    = lk_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    lk_error_(f, "pipeline execution refusal");
    return;
  }
}

static void lk_error_(upd_file_t* f, const char* msg) {
  ctx_t_* ctx = f->ctx;

  msgpack_packer* pk = &ctx->mpk.pk;

  ctx->mpk.broken |=
    msgpack_pack_map(pk, 2)                ||
      upd_msgpack_pack_cstr(pk, "success") ||
      msgpack_pack_false(pk)               ||

      upd_msgpack_pack_cstr(pk, "msg") ||
      upd_msgpack_pack_cstr(pk, msg);

  if (ctx->locked && !ctx->linked) {
    lk_clear_(f);
    ctx->locked = false;
    upd_file_unlock(&ctx->lock);
  }

  upd_file_trigger(f, UPD_FILE_UPDATE);
  lk_msgpack_cb_(&ctx->mpk);
}

static bool lk_assign_input_(
    upd_file_t* f, const gra_gl3_pl_var_t* var, const msgpack_object* obj) {
  upd_iso_t* iso = f->iso;
  ctx_t_*    ctx = f->ctx;

  size_t dim = var->type & GRA_GL3_PL_VAR_DIM_MASK;

  void* ptr = ctx->varbuf + var->offset;
  switch (var->type) {
  case GRA_GL3_PL_VAR_INTEGER:
    switch (obj->type) {
    case MSGPACK_OBJECT_POSITIVE_INTEGER:
    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
      *(GLint*) ptr = obj->via.i64;
      return true;
    default:
      return false;
    }

  case GRA_GL3_PL_VAR_SCALAR:
    switch (obj->type) {
    case MSGPACK_OBJECT_FLOAT:
      *(GLfloat*) ptr = obj->via.f64;
      return true;
    default:
      return false;
    }

  case GRA_GL3_PL_VAR_MAT2:
  case GRA_GL3_PL_VAR_MAT3:
  case GRA_GL3_PL_VAR_MAT4:
    dim *= dim;
    HEDLEY_FALL_THROUGH;

  case GRA_GL3_PL_VAR_VEC2:
  case GRA_GL3_PL_VAR_VEC3:
  case GRA_GL3_PL_VAR_VEC4: {
    if (HEDLEY_UNLIKELY(obj->type != MSGPACK_OBJECT_ARRAY)) {
      return false;
    }
    const msgpack_object_array* a = &obj->via.array;
    if (HEDLEY_UNLIKELY(a->size != dim)) {
      return false;
    }
    for (size_t i = 0; i < dim; ++i) {
      if (HEDLEY_UNLIKELY(a->ptr[i].type != MSGPACK_OBJECT_FLOAT)) {
        return false;
      }
      *((GLfloat*) ptr + i) = a->ptr[i].via.f64;
    }
  } return true;

  default:
    if (HEDLEY_UNLIKELY(obj->type != MSGPACK_OBJECT_POSITIVE_INTEGER)) {
      return false;
    }
    upd_file_t* src = upd_file_get(iso, obj->via.u64);
    if (HEDLEY_UNLIKELY(src == NULL)) {
      return false;
    }
    return lk_assign_input_file_(f, var, src);
  }
}

static bool lk_assign_input_file_(
    upd_file_t* f, const gra_gl3_pl_var_t* var, upd_file_t* src) {
  upd_iso_t* iso = f->iso;
  ctx_t_*    ctx = f->ctx;

  const upd_driver_t* d = gra_gl3_get_driver_from_var_type(var->type);

  if (HEDLEY_UNLIKELY(src->driver != d)) {
    upd_file_t* dst = upd_file_new(&(upd_file_t) {
        .iso    = iso,
        .driver = d,
      });
    if (HEDLEY_UNLIKELY(dst == NULL)) {
      return false;
    }

    external_t_* ext = NULL;
    if (HEDLEY_UNLIKELY(!upd_malloc(&ext, sizeof(*ext)))) {
      upd_file_unref(dst);
      return false;
    }
    *ext = (external_t_) {
      .lk  = f,
      .var = var,
      .src = src,
      .dst = dst,
    };
    if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->external, ext, SIZE_MAX))) {
      upd_free(&ext);
      upd_file_unref(dst);
      return false;
    }
    if (HEDLEY_UNLIKELY(!lk_assign_input_file_(f, var, dst))) {
      upd_array_find_and_remove(&ctx->external, ext);
      upd_free(&ext);
      upd_file_unref(dst);
      return false;
    }
    upd_file_ref(src);
    return true;
  }

  size_t temp = 0;
  if (HEDLEY_LIKELY(!upd_array_find(&ctx->in, &temp, src))) {
    if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->in, src, SIZE_MAX))) {
      return false;
    }
    upd_file_ref(src);
  }

  assign_t_* ass = upd_iso_stack(iso, sizeof(*ass));
  if (HEDLEY_UNLIKELY(ass == NULL)) {
    return false;
  }
  *ass = (assign_t_) {
    .lk   = f,
    .var  = var,
    .file = src,
    .lock = {
      .file  = src,
      .udata = ass,
      .cb    = assign_lock_input_cb_,
    },
  };

  ++ctx->refcnt;
  if (HEDLEY_UNLIKELY(!upd_file_lock(&ass->lock))) {
    upd_iso_unstack(iso, ass);
    --ctx->refcnt;
    return false;
  }
  return true;
}

static bool lk_create_and_assign_output_tex_(
    upd_file_t* f, const gra_gl3_pl_out_t* out) {
  upd_iso_t* iso = f->iso;
  ctx_t_*    ctx = f->ctx;

  upd_file_t* gl = upd_file_new(&(upd_file_t) {
      .iso    = iso,
      .driver = gra_gl3_get_driver_from_var_type(out->var->type),
    });
  if (HEDLEY_UNLIKELY(gl == NULL)) {
    return false;
  }

  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->out, gl, SIZE_MAX))) {
    upd_file_unref(gl);
    return false;
  }

  assign_t_* ass = upd_iso_stack(iso, sizeof(*ass));
  if (HEDLEY_UNLIKELY(ass == NULL)) {
    return false;
  }
  *ass = (assign_t_) {
    .lk   = f,
    .var  = out->var,
    .file = gl,
    .lock = {
      .file  = gl,
      .ex    = true,
      .udata = ass,
      .cb    = assign_lock_output_cb_,
    },
  };

  ++ctx->refcnt;
  if (HEDLEY_UNLIKELY(!upd_file_lock(&ass->lock))) {
    --ctx->refcnt;
    upd_iso_unstack(iso, ass);
    return false;
  }
  return true;
}

static void lk_msgpack_cb_(upd_msgpack_t* mpk) {
  upd_file_t* f   = mpk->udata;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!mpk->busy)) {
    msgpack_unpacked_init(&ctx->upkd);
  }

  if (HEDLEY_UNLIKELY(!upd_msgpack_pop(mpk, &ctx->upkd))) {
    msgpack_unpacked_destroy(&ctx->upkd);
    if (HEDLEY_UNLIKELY(mpk->broken)) {
      upd_file_trigger(f, UPD_FILE_UPDATE);
    }
    if (mpk->busy) {
      mpk->busy = false;
      upd_file_unref(f);
    }
    return;
  }

  if (HEDLEY_UNLIKELY(!mpk->busy)) {
    mpk->busy = true;
    upd_file_ref(f);
  }
  (ctx->linked? lk_handle_exec_: lk_handle_link_)(f);
}

static void lk_lock_pl_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    lk_error_(f, "target lock cancelled");
    return;
  }
  ctx->locked = true;

  const bool fetch = gra_gl3_lock_and_fetch_with_dup(&(gra_gl3_fetch_t) {
      .file  = k->file,
      .type  = GRA_GL3_FETCH_PL,
      .udata = f,
      .cb    = lk_fetch_pl_cb_,
    });
  if (HEDLEY_UNLIKELY(!fetch)) {
    lk_error_(f, "target fetch refusal");
    return;
  }
}

static void lk_fetch_pl_cb_(gra_gl3_fetch_t* fe) {
  upd_file_t* f   = fe->udata;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  const gra_gl3_pl_t* pl = fe->ok? fe->pl: NULL;
  upd_file_unlock(&fe->lock);
  upd_iso_unstack(iso, fe);

  if (HEDLEY_UNLIKELY(pl == NULL)) {
    lk_error_(f, "target fetch failure");
    return;
  }
  ctx->pl = pl;

  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx->varbuf, pl->varbuflen))) {
    lk_error_(f, "varbuf allocation failure");
    return;
  }
  memset(ctx->varbuf, 0, pl->varbuflen);

  const msgpack_object_map* param = NULL;
  const char* invalid =
    upd_msgpack_find_fields(&ctx->upkd.data.via.map, (upd_msgpack_field_t[]) {
        { .name = "param", .map = &param, },
        { NULL, },
      });
  if (HEDLEY_UNLIKELY(invalid)) {
    lk_error_(f, "invalid req for execution");
    return;
  }

  ctx->refcnt = 1;

  /* input parameters */
  if (param) {
    for (size_t i = 0; i < pl->varcnt; ++i) {
      const gra_gl3_pl_var_t* var = &pl->var[i];
      if (!var->in) continue;

      const msgpack_object* p =
        upd_msgpack_find_obj_by_str(param, var->name, utf8size_lazy(var->name));
      if (p == NULL) continue;

      if (HEDLEY_UNLIKELY(!lk_assign_input_(f, var, p))) {
        ctx->aborted = true;
        break;
      }
    }
  }

  /* output textures */
  for (size_t i = 0; i < pl->outcnt; ++i) {
    if (HEDLEY_UNLIKELY(!lk_create_and_assign_output_tex_(f, &pl->out[i]))) {
      ctx->aborted = true;
      break;
    }
  }

  lk_unref_link_(f);
}

static void lk_link_cb_(gra_gl3_req_t* req) {
  upd_file_t* f   = req->udata;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  const bool ok = req->ok;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!ok)) {
    lk_error_(f, "pipeline link failure");
    return;
  }

  ctx->linked = true;

  msgpack_packer* pk = &ctx->mpk.pk;
  ctx->mpk.broken =
    msgpack_pack_map(pk, 2)                ||
      upd_msgpack_pack_cstr(pk, "success") ||
        msgpack_pack_true(pk)              ||
      upd_msgpack_pack_cstr(pk, "output")  ||
        msgpack_pack_map(pk, ctx->out.n);

  assert(ctx->out.n == ctx->pl->outcnt);
  for (size_t i = 0; i < ctx->out.n; ++i) {
    upd_file_t* of = ctx->out.p[i];
    ctx->mpk.broken |=
      upd_msgpack_pack_cstr(pk, (char*) ctx->pl->out[i].var->name) ||
      msgpack_pack_uint64(pk, of->id);
  }

  upd_file_trigger(f, UPD_FILE_UPDATE);
  lk_msgpack_cb_(&ctx->mpk);
}

static void lk_lock_for_exec_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->reslock, k, SIZE_MAX))) {
    goto ABORT;
  }
  goto EXIT;

ABORT:
  ctx->aborted = true;
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);

EXIT:
  lk_unref_exec_(f);
}

static void lk_exec_cb_(gra_gl3_req_t* req) {
  upd_file_t*     f   = req->udata;
  upd_iso_t*      iso = f->iso;
  ctx_t_*         ctx = f->ctx;
  msgpack_packer* pk  = &ctx->mpk.pk;

  const bool ok = req->ok;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!ok)) {
    lk_error_(f, "pipeline execution failure");
    return;
  }

  for (size_t i = 0; i < ctx->reslock.n; ++i) {
    upd_file_lock_t* k = ctx->reslock.p[i];
    upd_file_unlock(k);
    upd_iso_unstack(iso, k);
  }
  upd_array_clear(&ctx->reslock);

  ctx->mpk.broken =
    msgpack_pack_map(pk, 1)                ||
      upd_msgpack_pack_cstr(pk, "success") ||
        msgpack_pack_true(pk);

  upd_file_trigger(f, UPD_FILE_UPDATE);
  lk_msgpack_cb_(&ctx->mpk);
}

static void lk_unlink_cb_(gra_gl3_req_t* req) {
  ctx_t_*    ctx = req->udata;
  upd_iso_t* iso = ctx->def->iso;

  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
  lk_teardown_(ctx);
}


static void assign_lock_input_cb_(upd_file_lock_t* k) {
  assign_t_*  ass = k->udata;
  upd_file_t* f   = ass->lk;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  GLuint id = 0;
  if (HEDLEY_UNLIKELY(!k->ok)) {
    goto EXIT;
  }

  if (ass->var->type & GRA_GL3_PL_VAR_TEX_MASK) {
    gra_gl3_tex_t* tex = ass->file->ctx;
    id = tex->id;

  } else if (ass->var->type & GRA_GL3_PL_VAR_BUF_MASK) {
    gra_gl3_buf_t* buf = ass->file->ctx;
    id = buf->id;

  } else {
    assert(false);
  }

  if (HEDLEY_UNLIKELY(id == 0)) {
    ctx->aborted = true;
  } else {
    *(GLuint*) (ctx->varbuf + ass->var->offset) = id;
  }

EXIT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, ass);

  lk_unref_link_(f);
}

static void assign_lock_output_cb_(upd_file_lock_t* k) {
  assign_t_*    ass = k->udata;
  upd_file_t*   f   = ass->lk;
  upd_iso_t*    iso = f->iso;
  ctx_t_*       ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    goto ABORT;
  }

  const gra_gl3_pl_t*     pl  = ctx->pl;
  const gra_gl3_pl_var_t* var = ass->var;
  const gra_gl3_pl_out_t* out = &pl->out[var->index];

  gra_gl3_tex_t* tex = ass->file->ctx;
  if (HEDLEY_UNLIKELY(tex->id == 0)) {
    goto ABORT;
  }
  *(GLuint*) (ctx->varbuf + var->offset) = tex->id;

  const size_t rank = (var->type & GRA_GL3_PL_VAR_DIM_MASK)+1;
  assert(2 <= rank && rank <= 4);

  for (size_t i = 0; i < rank; ++i) {
    intmax_t v = 0;
    const bool ok = gra_gl3_pl_get_value(
      &out->reso[i], ctx->varbuf, &v, NULL);
    if (HEDLEY_UNLIKELY(!ok || v <= 0)) {
      goto ABORT;
    }
    ass->reso[i] = v;
  }

  ass->greq = (gra_gl3_req_t) {
    .dev  = tex->gl,
    .type = GRA_GL3_REQ_TEX_ALLOC,
    .tex  = {
      .id     = tex->id,
      .target = gra_gl3_rank_to_tex_target(rank),
      .fmt    = gra_gl3_dim_to_color_fmt(ass->reso[0]),
      .type   = GL_UNSIGNED_BYTE,
      .w      = ass->reso[1],
      .h      = ass->reso[2],
      .d      = ass->reso[3],
    },
    .udata = ass,
    .cb    = assign_alloc_output_cb_,
  };
  gra_gl3_tex_set_metadata(ass->file, &ass->greq);

  if (HEDLEY_UNLIKELY(!gra_gl3_lock_and_req(&ass->greq))) {
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, ass);

  lk_unref_link_(f);
}

static void assign_alloc_output_cb_(gra_gl3_req_t* greq) {
  assign_t_*  ass = greq->udata;
  upd_file_t* f   = ass->lk;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!greq->ok)) {
    ctx->aborted = true;
  }

  upd_file_unlock(&ass->greq.lock);
  upd_file_unlock(&ass->lock);
  upd_iso_unstack(iso, ass);

  lk_unref_link_(f);
}


static void copy_finalize_(copy_t_* cpy) {
  upd_file_t* f   = cpy->ext->lk;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_LIKELY(cpy->src_fetched)) {
    cpy->req = (upd_req_t) {
      .file  = cpy->ext->src,
      .type  = UPD_REQ_TENSOR_FLUSH,
      .udata = cpy,
      .cb    = copy_flush_src_cb_,
    };
    if (HEDLEY_LIKELY(upd_req(&cpy->req))) {
      return;
    }
  }
  if (HEDLEY_LIKELY(cpy->src_locked)) {
    upd_file_unlock(&cpy->src_lock);
  }
  if (HEDLEY_LIKELY(cpy->dst_locked)) {
    upd_file_unlock(&cpy->dst_lock);
  }

  upd_iso_unstack(iso, cpy);
  lk_unref_exec_(f);
}

static void copy_lock_dst_cb_(upd_file_lock_t* k) {
  copy_t_*    cpy = k->udata;
  upd_file_t* f   = cpy->ext->lk;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    goto ABORT;
  }
  cpy->dst_locked = true;

  cpy->src_lock = (upd_file_lock_t) {
    .file  = cpy->ext->src,
    .udata = cpy,
    .cb    = copy_lock_src_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&cpy->src_lock))) {
    goto ABORT;
  }
  return;

ABORT:
  ctx->aborted = true;
  copy_finalize_(cpy);
}

static void copy_lock_src_cb_(upd_file_lock_t* k) {
  copy_t_*    cpy = k->udata;
  upd_file_t* f   = cpy->ext->lk;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    goto ABORT;
  }
  cpy->src_locked = true;

  cpy->req = (upd_req_t) {
    .file  = cpy->ext->src,
    .type  = UPD_REQ_TENSOR_FETCH,
    .udata = cpy,
    .cb    = copy_fetch_src_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&cpy->req))) {
    goto ABORT;
  }
  return;

ABORT:
  ctx->aborted = true;
  copy_finalize_(cpy);
}

static void copy_fetch_src_cb_(upd_req_t* req) {
  copy_t_*    cpy = req->udata;
  upd_file_t* f   = cpy->ext->lk;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    goto ABORT;
  }
  cpy->src_fetched = true;

  const gra_gl3_pl_var_t* var = cpy->ext->var;

  const upd_req_tensor_data_t* data = &req->tensor.data;
  const upd_req_tensor_meta_t* meta = &data->meta;

  if (HEDLEY_UNLIKELY(meta->type == UPD_TENSOR_F64)) {
    goto ABORT;
  }

  if (cpy->ext->var->type & GRA_GL3_PL_VAR_TEX_MASK) {
    gra_gl3_tex_t* tex = cpy->ext->dst->ctx;
    if (HEDLEY_UNLIKELY(tex->id == 0)) {
      goto ABORT;
    }
    if (HEDLEY_UNLIKELY(meta->rank != tex->rank)) {
      goto ABORT;
    }

    cpy->greq = (gra_gl3_req_t) {
      .dev  = tex->gl,
      .type = GRA_GL3_REQ_TEX_ALLOC,
      .tex = {
        .id     = tex->id,
        .target = tex->target,
        .type   = gra_gl3_tensor_type_to_type(meta->type),
        .fmt    = gra_gl3_dim_to_color_fmt(meta->reso[0]),

        .w = meta->reso[1],
        .h = meta->rank >= 3? meta->reso[2]: 0,
        .d = meta->rank >= 4? meta->reso[3]: 0,

        .data = data->ptr,
      },
      .udata = cpy,
      .cb    = copy_alloc_dst_cb_,
    };
    if (HEDLEY_UNLIKELY(!gra_gl3_req(&cpy->greq))) {
      goto ABORT;
    }
    return;

  } else if (cpy->ext->var->type & GRA_GL3_PL_VAR_BUF_MASK) {
    gra_gl3_buf_t* buf = cpy->ext->dst->ctx;
    if (HEDLEY_UNLIKELY(buf->id == 0)) {
      goto ABORT;
    }

    *(GLuint*) (ctx->varbuf + var->offset) = buf->id;

    const size_t size =
      upd_tensor_count_scalars(meta)*upd_tensor_type_sizeof(meta->type);

    cpy->greq = (gra_gl3_req_t) {
      .dev  = buf->gl,
      .type = GRA_GL3_REQ_BUF_ALLOC,
      .buf_alloc = {
        .id     = buf->id,
        .target = buf->target,
        .usage  = GL_DYNAMIC_DRAW,
        .data   = data->ptr,
        .size   = size,
      },
      .udata = cpy,
      .cb    = copy_alloc_dst_cb_,
    };
    if (HEDLEY_UNLIKELY(!gra_gl3_req(&cpy->greq))) {
      goto ABORT;
    }
    return;

  } else {
    assert(false);
  }

ABORT:
  ctx->aborted = true;
  copy_finalize_(cpy);
}

static void copy_alloc_dst_cb_(gra_gl3_req_t* req) {
  copy_t_*    cpy = req->udata;
  upd_file_t* f   = cpy->ext->lk;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!req->ok)) {
    ctx->aborted = true;
  }
  copy_finalize_(cpy);
}

static void copy_flush_src_cb_(upd_req_t* req) {
  copy_t_* cpy = req->udata;

  cpy->src_fetched = false;
  copy_finalize_(cpy);
}
