#include "common.h"

#define PARSE_DEPTH_MAX_ 8


typedef struct parse_t_        parse_t_;
typedef struct parse_shfind_t_ parse_shfind_t_;

typedef struct compile_t_      compile_t_;
typedef struct compile_draw_t_ compile_draw_t_;


struct parse_t_ {
  upd_file_t* def;

  upd_file_t*        stream;
  upd_msgpack_recv_t recv;

  upd_file_lock_t lock;
  upd_req_t       req;

  size_t refcnt;

  unsigned ok        : 1;
  unsigned locked    : 1;
  unsigned recv_init : 1;

  void* udata;
  void
  (*cb)(
    parse_t_* par);

  const char* error;
  size_t      depth;
  size_t      loc[PARSE_DEPTH_MAX_];
};

struct parse_shfind_t_ {
  parse_t_*            par;
  gra_gl3_step_draw_t* draw;

  const uint8_t* path;
  size_t         pathlen;

  upd_pathfind_t pf;
};


struct compile_t_ {
  upd_file_t* def;

  size_t refcnt;

  unsigned ok : 1;

  void* udata;
  void
  (*cb)(
    compile_t_* cp);
};

struct compile_draw_t_ {
  compile_t_*          cp;
  gra_gl3_step_draw_t* draw;

  size_t index;

  gra_gl3_req_t   req;
  gra_gl3_fetch_t fe;
};


static
bool
def_init_(
  upd_file_t* f);

static
void
def_deinit_(
  upd_file_t* f);

static
bool
def_handle_(
  upd_req_t* req);

HEDLEY_PRINTF_FORMAT(2, 3)
static
void
def_logf_(
  upd_file_t* f,
  const char* msg,
  ...);

static
void
def_teardown_cache_(
  upd_file_t* f);

static
void
def_finalize_fetch_(
  upd_file_t* f,
  bool        ok);

const upd_driver_t gra_gl3_pl_def = {
  .name   = (uint8_t*) "upd.graphics.gl3.pipeline.def",
  .cats   = (upd_req_cat_t[]) {
    0,
  },
  .init   = def_init_,
  .deinit = def_deinit_,
  .handle = def_handle_,
};


static
bool
parse_(
  parse_t_* par);

static
bool
parse_with_dup_(
  const parse_t_* src);

static
void
parse_unref_(
  parse_t_* par);

static
void
parse_stack_(
  parse_t_* par,
  size_t    index);

HEDLEY_WARN_UNUSED_RESULT
static
bool
parse_in_(
  parse_t_*                 par,
  const msgpack_object_map* src);

HEDLEY_WARN_UNUSED_RESULT
static
bool
parse_fb_(
  parse_t_*                 par,
  const msgpack_object_map* src);

HEDLEY_WARN_UNUSED_RESULT
static
bool
parse_va_(
  parse_t_*                 par,
  const msgpack_object_map* src);

HEDLEY_WARN_UNUSED_RESULT
static
bool
parse_step_(
  parse_t_*                   par,
  const msgpack_object_array* src);

HEDLEY_WARN_UNUSED_RESULT
static
bool
parse_value_(
  parse_t_*             par,
  gra_gl3_pl_value_t*   v,
  const msgpack_object* obj);

static
bool
parse_shfind_with_dup_(
  const parse_shfind_t_* src);


static
void
compile_(
  compile_t_* cp);

static
void
compile_unref_(
  compile_t_* cp);

static
void
compile_draw_fetch_and_attach_next_shader_(
  compile_draw_t_* cpd);


static
void
def_init_lock_cb_(
  upd_file_lock_t* k);

static
void
def_init_pathfind_cb_(
  upd_pathfind_t* pf);

static
void
def_watch_obj_and_sh_cb_(
  upd_file_watch_t* w);

static
void
def_parse_cb_(
  parse_t_* par);

static
void
def_compile_cb_(
  compile_t_* cp);

static
void
def_gl_prog_delete_cb_(
  gra_gl3_req_t* req);


static
void
parse_lock_obj_cb_(
  upd_file_lock_t* k);

static
void
parse_exec_obj_cb_(
  upd_req_t* req);

static
void
parse_lock_stream_for_write_cb_(
  upd_file_lock_t* k);

static
void
parse_write_stream_cb_(
  upd_req_t* req);

static
void
parse_recv_lock_cb_(
  upd_msgpack_recv_t* recv);

static
void
parse_recv_get_cb_(
  upd_msgpack_recv_t* recv);

static
void
parse_shfind_pathfind_cb_(
  upd_pathfind_t* pf);


static
void
compile_draw_gl_prog_create_cb_(
  gra_gl3_req_t* req);

static
void
compile_draw_fetch_shader_cb_(
  gra_gl3_fetch_t* fe);

static
void
compile_draw_gl_prog_attach_cb_(
  gra_gl3_req_t* req);

static
void
compile_draw_gl_prog_link_cb_(
  gra_gl3_req_t* req);


bool gra_gl3_pl_def_fetch(gra_gl3_fetch_t* fe) {
  upd_file_t* f = fe->file;
  gra_gl3_pl_def_t* ctx = f->ctx;

  fe->ok = false;
  if (HEDLEY_UNLIKELY(!ctx->gl)) {
    return false;
  }

  if (HEDLEY_LIKELY(ctx->clean)) {
    fe->ok = !!ctx->pl;
    fe->pl = ctx->pl;
    fe->cb(fe);
    return true;
  }

  const bool busy = ctx->pending.n;
  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->pending, fe, SIZE_MAX))) {
    return false;
  }
  if (HEDLEY_UNLIKELY(busy)) {
    return true;
  }

  const bool parse = parse_with_dup_(&(parse_t_) {
      .def   = fe->file,
      .udata = fe,
      .cb    = def_parse_cb_,
    });
  if (HEDLEY_UNLIKELY(!parse)) {
    upd_array_clear(&ctx->pending);
    return false;
  }
  return true;
}


static bool def_init_(upd_file_t* f) {
  if (HEDLEY_UNLIKELY(f->npath == NULL)) {
    def_logf_(f, "empty npath");
    return false;
  }
  if (HEDLEY_UNLIKELY(f->backend == NULL)) {
    def_logf_(f, "requires backend file");
    return false;
  }

  gra_gl3_pl_def_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    def_logf_(f, "context allocation failure");
    return false;
  }
  *ctx = (gra_gl3_pl_def_t) {
    .watch = {
      .file  = f->backend,
      .udata = f,
      .cb    = def_watch_obj_and_sh_cb_,
    },
  };
  f->ctx = ctx;
  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    return false;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .ex    = true,
      .udata = f,
      .cb    = def_init_lock_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    return false;
  }
  return true;
}

static void def_deinit_(upd_file_t* f) {
  gra_gl3_pl_def_t* ctx = f->ctx;

  upd_file_unwatch(&ctx->watch);

  def_teardown_cache_(f);
  if (HEDLEY_LIKELY(ctx->gl)) {
    upd_file_unref(ctx->gl);
  }

  upd_free(&ctx->pl);
  upd_free(&ctx);
}

static bool def_handle_(upd_req_t* req) {
  upd_file_t* f = req->file;
  req->file = f->backend;
  return upd_req(req);
}

static void def_logf_(upd_file_t* f, const char* fmt, ...) {
  uint8_t temp[256];

  va_list args;
  va_start(args, fmt);
  vsnprintf((char*) temp, sizeof(temp), fmt, args);
  va_end(args);

  upd_iso_msgf(f->iso, "upd.graphics.gl3.pipeline.def: %s (%s)\n", temp, f->npath);
}

static void def_teardown_cache_(upd_file_t* f) {
  upd_iso_t*        iso = f->iso;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  for (size_t i = 0; i < ctx->shwatch.n; ++i) {
    upd_file_watch_t* w = ctx->shwatch.p[i];
    upd_file_unwatch(w);
    upd_file_unref(w->file);
    upd_free(&w);
  }
  upd_array_clear(&ctx->shwatch);

  if (HEDLEY_UNLIKELY(pl == NULL)) {
    return;
  }

  for (size_t i = 0; i < pl->stepcnt; ++i) {
    if (HEDLEY_UNLIKELY(pl->step[i].type != GRA_GL3_STEP_DRAW)) {
      continue;
    }

    const gra_gl3_step_draw_t* draw = &pl->step[i].draw;
    if (HEDLEY_UNLIKELY(draw->prog == 0)) {
      continue;
    }

    const bool ok = gra_gl3_lock_and_req_with_dup(&(gra_gl3_req_t) {
        .dev  = ctx->gl,
        .type = GRA_GL3_REQ_PROG_DELETE,
        .prog = {
          .id = draw->prog,
        },
        .udata = iso,
        .cb    = def_gl_prog_delete_cb_,
      });
    if (HEDLEY_UNLIKELY(!ok)) {
      def_logf_(f, "GL3_REQ_PROG_DELETE failure, some OpenGL program may leak");
    }
  }
}

static void def_finalize_fetch_(upd_file_t* f, bool ok) {
  gra_gl3_pl_def_t* ctx = f->ctx;

  if (HEDLEY_UNLIKELY(!ok)) {
    def_teardown_cache_(f);
    upd_free(&ctx->pl);
  }

  upd_array_t pens = ctx->pending;
  ctx->pending = (upd_array_t) {0};
  ctx->clean   = true;

  for (size_t i = 0; i < pens.n; ++i) {
    gra_gl3_fetch_t* fe = pens.p[i];
    fe->pl = ctx->pl;
    fe->ok = ok;
    fe->cb(fe);
  }
  upd_array_clear(&pens);
}


static bool parse_(parse_t_* par) {
  upd_file_t* f = par->def;

  par->refcnt = 1;

  par->lock = (upd_file_lock_t) {
    .file  = f->backend,
    .udata = par,
    .cb    = parse_lock_obj_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&par->lock))) {
    def_logf_(f, "backend file lock refusal");
    return false;
  }
  return true;
}

static bool parse_with_dup_(const parse_t_* src) {
  upd_file_t* f   = src->def;
  upd_iso_t*  iso = f->iso;

  parse_t_* par = upd_iso_stack(iso, sizeof(*par));
  if (HEDLEY_UNLIKELY(par == NULL)) {
    return false;
  }
  *par = *src;
  if (HEDLEY_UNLIKELY(!parse_(par))) {
    upd_iso_unstack(iso, par);
    return false;
  }
  return true;
}

static void parse_unref_(parse_t_* par) {
  if (HEDLEY_UNLIKELY(--par->refcnt)) {
    return;
  }

  if (HEDLEY_LIKELY(par->locked)) {
    upd_file_unlock(&par->lock);
  }
  if (HEDLEY_UNLIKELY(par->recv_init)) {
    upd_msgpack_recv_deinit(&par->recv);
  }
  if (HEDLEY_LIKELY(par->stream)) {
    upd_file_unref(par->stream);
  }
  par->cb(par);
}

static void parse_stack_(parse_t_* par, size_t index) {
  if (HEDLEY_UNLIKELY(par->depth < PARSE_DEPTH_MAX_)) {
    par->loc[par->depth] = index;
  }
  ++par->depth;
}

static gra_gl3_pl_var_t* parse_var_name_(
    parse_t_* par, const msgpack_object* src) {
  upd_file_t*       f   = par->def;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  if (HEDLEY_UNLIKELY(src->type != MSGPACK_OBJECT_STR)) {
    par->error = "found non-string var name";
    return NULL;
  }

  const msgpack_object_str* k = &src->via.str;
  if (HEDLEY_UNLIKELY(k->size >= GRA_GL3_PL_IDENT_MAX)) {
    par->error = "too long var name";
    return NULL;
  }
  if (HEDLEY_UNLIKELY(k->size == 0)) {
    par->error = "empty var name";
    return NULL;
  }
  if (HEDLEY_UNLIKELY(gra_gl3_pl_find_var(pl, (uint8_t*) k->ptr, k->size))) {
    par->error = "var name duplicated";
    return NULL;
  }

  gra_gl3_pl_var_t* var = &pl->var[pl->varcnt++];
  *var = (gra_gl3_pl_var_t) {0};
  utf8ncpy(var->name, k->ptr, k->size);
  return var;
}

static bool parse_in_(parse_t_* par, const msgpack_object_map* src) {
  upd_file_t*       f   = par->def;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  const size_t cnt = src? src->size: 0;
  for (size_t i = 0; i < cnt; ++i) {
    parse_stack_(par, i);

    const msgpack_object_kv* kv = &src->ptr[i];
    if (HEDLEY_UNLIKELY(kv->val.type != MSGPACK_OBJECT_STR)) {
      par->error = "expected string";
      return false;
    }

    gra_gl3_pl_var_t* var = parse_var_name_(par, &kv->key);
    if (HEDLEY_UNLIKELY(var == NULL)) {
      return false;
    }
    var->in     = true;
    var->index  = i;
    var->offset = pl->varbuflen;

    const msgpack_object_str* v = &kv->val.via.str;
    var->type = gra_gl3_pl_var_type_unstringify((uint8_t*) v->ptr, v->size);
    if (HEDLEY_UNLIKELY(var->type == GRA_GL3_PL_VAR_NONE)) {
      par->error = "invalid var type specification";
      return false;
    }

    pl->varbuflen += gra_gl3_pl_sizeof_var(var->type);

    --par->depth;
  }
  return true;
}

static bool parse_out_(parse_t_* par, const msgpack_object_map* src) {
  upd_file_t*       f   = par->def;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  const size_t cnt = src? src->size: 0;
  for (size_t i = 0; i < cnt; ++i) {
    parse_stack_(par, i);

    const msgpack_object_kv* kv = &src->ptr[i];
    if (HEDLEY_UNLIKELY(kv->val.type != MSGPACK_OBJECT_ARRAY)) {
      par->error = "expected array";
      return false;
    }

    gra_gl3_pl_var_t* var = parse_var_name_(par, &kv->key);
    if (HEDLEY_UNLIKELY(var == NULL)) {
      return false;
    }
    var->index  = pl->outcnt;
    var->offset = pl->varbuflen;

    gra_gl3_pl_out_t* out = &pl->out[pl->outcnt++];
    *out = (gra_gl3_pl_out_t) {
      .var = var,
    };

    const msgpack_object_array* v = &kv->val.via.array;
    if (HEDLEY_UNLIKELY(v->size < 2 || 4 < v->size)) {
      par->error = "texture dimension must be 2~4";
      return false;
    }

    var->type =
      v->size == 2? GRA_GL3_PL_VAR_TEX1:
      v->size == 3? GRA_GL3_PL_VAR_TEX2:
      v->size == 4? GRA_GL3_PL_VAR_TEX3:
      0;

    pl->varbuflen += gra_gl3_pl_sizeof_var(var->type);

    for (size_t j = 0; j < v->size; ++j) {
      parse_stack_(par, j);

      gra_gl3_pl_value_t* val = &out->reso[j];
      if (HEDLEY_UNLIKELY(!parse_value_(par, val, &v->ptr[j]))) {
        return false;
      }

      const gra_gl3_pl_var_type_t t =
        gra_gl3_pl_get_value_entity_type(val);
      if (HEDLEY_UNLIKELY(t != GRA_GL3_PL_VAR_INTEGER)) {
        par->error = "texture resolution must be integer";
        return false;
      }

      --par->depth;
    }

    --par->depth;
  }
  return true;
}

static bool parse_fb_(parse_t_* par, const msgpack_object_map* src) {
  upd_file_t*       f   = par->def;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  const size_t cnt = src? src->size: 0;
  for (size_t i = 0; i < cnt; ++i) {
    parse_stack_(par, i);

    const msgpack_object_kv* kv = &src->ptr[i];
    if (HEDLEY_UNLIKELY(kv->val.type != MSGPACK_OBJECT_MAP)) {
      par->error = "expected map";
      return false;
    }

    gra_gl3_pl_var_t* var = parse_var_name_(par, &kv->key);
    if (HEDLEY_UNLIKELY(var == NULL)) {
      return false;
    }
    var->type   = GRA_GL3_PL_VAR_FB;
    var->index  = pl->fbcnt;
    var->offset = pl->varbuflen;

    pl->varbuflen += gra_gl3_pl_sizeof_var(var->type);

    gra_gl3_pl_fb_t* fb = &pl->fb[pl->fbcnt++];
    *fb = (gra_gl3_pl_fb_t) {
      .var = var,
    };

    const msgpack_object_map* v = &kv->val.via.map;
    for (size_t j = 0; j < v->size; ++j) {
      parse_stack_(par, j);

      const msgpack_object_kv* kv = &v->ptr[j];
      if (HEDLEY_UNLIKELY(kv->key.type != MSGPACK_OBJECT_STR)) {
        par->error = "non-string key found";
        return false;
      }
      if (HEDLEY_UNLIKELY(kv->val.type != MSGPACK_OBJECT_STR)) {
        par->error = "string expected";
        return false;
      }

      const msgpack_object_str* k2 = &v->ptr[i].key.via.str;
      const msgpack_object_str* v2 = &v->ptr[i].val.via.str;

      gra_gl3_pl_fb_attach_t* a = &fb->attach[fb->attachcnt++];

      const bool typevalid = gra_gl3_enum_unstringify_attachment(
        &a->type, (uint8_t*) k2->ptr, k2->size);
      if (HEDLEY_UNLIKELY(!typevalid)) {
        par->error = "unknown attachment type";
        return false;
      }

      a->var = gra_gl3_pl_find_var(pl, (uint8_t*) v2->ptr, v2->size);
      if (HEDLEY_UNLIKELY(a->var == NULL)) {
        par->error = "references unknown renderbuffer or texture";
        return false;
      }
      const bool vtypevalid =
        a->var->type == GRA_GL3_PL_VAR_RB ||
        a->var->type == GRA_GL3_PL_VAR_TEX2;
      if (HEDLEY_UNLIKELY(!vtypevalid)) {
        par->error = "references var with incompatible type";
        return false;
      }

      --par->depth;
    }

    --par->depth;
  }
  return true;
}

static bool parse_va_(parse_t_* par, const msgpack_object_map* src) {
  upd_file_t*       f   = par->def;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  const size_t cnt = src? src->size: 0;
  for (size_t i = 0; i < cnt; ++i) {
    parse_stack_(par, i);

    const msgpack_object_kv* kv = &src->ptr[i];

    if (HEDLEY_UNLIKELY(kv->val.type != MSGPACK_OBJECT_MAP)) {
      par->error = "expected map";
      return false;
    }

    gra_gl3_pl_var_t* var = parse_var_name_(par, &kv->key);
    if (HEDLEY_UNLIKELY(var == NULL)) {
      return false;
    }
    var->type   = GRA_GL3_PL_VAR_VA;
    var->index  = pl->vacnt;
    var->offset = pl->varbuflen;

    pl->varbuflen += gra_gl3_pl_sizeof_var(var->type);

    gra_gl3_pl_va_t* va = &pl->va[pl->vacnt++];
    *va = (gra_gl3_pl_va_t) {
      .var = var,
    };

    const msgpack_object_map* v = &kv->val.via.map;
    for (size_t j = 0; j < v->size; ++j) {
      parse_stack_(par, j);

      const msgpack_object_kv* kv = &v->ptr[j];
      if (HEDLEY_UNLIKELY(kv->key.type != MSGPACK_OBJECT_POSITIVE_INTEGER)) {
        par->error = "key must be positive integer";
        return false;
      }
      if (HEDLEY_UNLIKELY(kv->val.type != MSGPACK_OBJECT_MAP)) {
        par->error = "expected a map value";
        return false;
      }
      const size_t index = kv->key.via.u64;
      if (HEDLEY_UNLIKELY(index >= GRA_GL3_STEP_DRAW_IN_MAX)) {
        par->error = "vertex array index is too large";
        return false;
      }

      gra_gl3_pl_va_attach_t* a = &va->attach[index];

      const msgpack_object_str* sbuf    = NULL;
      const msgpack_object_str* type    = NULL;
      const msgpack_object*     stride  = NULL;
      const msgpack_object*     offset  = NULL;
      const msgpack_object*     divisor = NULL;
      uintmax_t                 dim     = 0;
      const char* invalid =
        upd_msgpack_find_fields(&kv->val.via.map, (upd_msgpack_field_t[]) {
            { .name = "dim",     .ui  = &dim,     .required = true,   },
            { .name = "buf",     .str = &sbuf,    .required = true,   },
            { .name = "type",    .str = &type,    .required = true,   },
            { .name = "stride",  .any = &stride,  .required = false,  },
            { .name = "offset",  .any = &offset,  .required = false,  },
            { .name = "divisor", .any = &divisor, .required = false,  },
            { NULL },
          });
      if (HEDLEY_UNLIKELY(invalid)) {
        par->error = "invalid vertexarray declaration";
        return false;
      }

      if (HEDLEY_UNLIKELY(dim == 0 || 4 < dim)) {
        par->error = "buffer dim must be 0~4";
        return false;
      }
      a->dim = dim;

      a->buf = gra_gl3_pl_find_var(pl, (uint8_t*) sbuf->ptr, sbuf->size);
      if (HEDLEY_UNLIKELY(a->buf == NULL)) {
        par->error = "refers unknown buffer";
        return false;
      }
      if (HEDLEY_UNLIKELY(a->buf->type != GRA_GL3_PL_VAR_BUF_ARRAY)) {
        par->error = "refers var with incompatible type as buffer";
        return false;
      }

      if (HEDLEY_UNLIKELY(type == NULL)) {
        par->error = "type is not specified";
        return false;
      }
      const bool typevalid = gra_gl3_enum_unstringify_buf_type(
        &a->type, (uint8_t*) type->ptr, type->size);
      if (HEDLEY_UNLIKELY(!typevalid)) {
        par->error = "invalid type";
        return false;
      }

      a->stride = (gra_gl3_pl_value_t) {
        .type = GRA_GL3_PL_VALUE_INTEGER,
        .i    = 0,
      };
      if (HEDLEY_UNLIKELY(stride)) {
        if (HEDLEY_UNLIKELY(!parse_value_(par, &a->stride, stride))) {
          return false;
        }
        const gra_gl3_pl_var_type_t t =
          gra_gl3_pl_get_value_entity_type(&a->stride);
        if (HEDLEY_UNLIKELY(t != GRA_GL3_PL_VAR_INTEGER)) {
          par->error = "'stride' must be integer value";
          return false;
        }
      }

      a->offset = (gra_gl3_pl_value_t) {
        .type = GRA_GL3_PL_VALUE_INTEGER,
        .i    = 0,
      };
      if (HEDLEY_UNLIKELY(offset)) {
        if (HEDLEY_UNLIKELY(!parse_value_(par, &a->offset, offset))) {
          return false;
        }
        const gra_gl3_pl_var_type_t t =
          gra_gl3_pl_get_value_entity_type(&a->offset);
        if (HEDLEY_UNLIKELY(t != GRA_GL3_PL_VAR_INTEGER)) {
          par->error = "'offset' must be integer value";
          return false;
        }
      }

      a->divisor = (gra_gl3_pl_value_t) {
        .type = GRA_GL3_PL_VALUE_INTEGER,
        .i    = 0,
      };
      if (HEDLEY_UNLIKELY(divisor)) {
        if (HEDLEY_UNLIKELY(!parse_value_(par, &a->divisor, divisor))) {
          return false;
        }
        const gra_gl3_pl_var_type_t t =
          gra_gl3_pl_get_value_entity_type(&a->divisor);
        if (HEDLEY_UNLIKELY(t != GRA_GL3_PL_VAR_INTEGER)) {
          par->error = "'divisor' must be integer value";
          return false;
        }
      }

      --par->depth;
    }

    --par->depth;
  }
  return true;
}

static bool parse_step_(parse_t_* par, const msgpack_object_array* src) {
  upd_file_t*       f   = par->def;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  const size_t cnt = src? src->size: 0;
  for (size_t i = 0; i < cnt; ++i) {
    parse_stack_(par, i);

    if (HEDLEY_UNLIKELY(src->ptr[i].type != MSGPACK_OBJECT_MAP)) {
      par->error = "expected map";
      return false;
    }
    const msgpack_object_map* map = &src->ptr[i].via.map;

    gra_gl3_step_t* step = &pl->step[pl->stepcnt++];
    *step = (gra_gl3_step_t) {0};

    const msgpack_object_str*   draw  = NULL;
    const msgpack_object_str*   srcfb = NULL;
    const msgpack_object_array* clear = NULL;

    const msgpack_object_str* dstfb = NULL;
    const msgpack_object_str* va    = NULL;

    const msgpack_object* count    = NULL;
    const msgpack_object* instance = NULL;

    const msgpack_object_map*   blend    = NULL;
    const msgpack_object_map*   depth    = NULL;
    const msgpack_object_array* viewport = NULL;

    const msgpack_object_map*   uni    = NULL;
    const msgpack_object_array* shader = NULL;

    const char* invalid = upd_msgpack_find_fields(map, (upd_msgpack_field_t[]) {
        /* type is decided by the following 3 fields */
        { .name = "clear", .array = &clear, },
        { .name = "draw",  .str   = &draw,  },
        { .name = "src",   .str   = &srcfb, },

        { .name = "dst",      .str   = &dstfb,    },
        { .name = "va",       .str   = &va,       },
        { .name = "count",    .any   = &count,    },
        { .name = "instance", .any   = &instance, },
        { .name = "blend",    .map   = &blend,    },
        { .name = "depth",    .map   = &depth,    },
        { .name = "viewport", .array = &viewport, },
        { .name = "uni",      .map   = &uni,      },
        { .name = "shader",   .array = &shader,   },
        { NULL },
      });
    if (HEDLEY_UNLIKELY(invalid)) {
      par->error = "invalid step item";
      return false;
    }
    if (HEDLEY_UNLIKELY(!!clear + !!draw + !!srcfb != 1)) {
      par->error =
        "cannot determine step type, specify one of draw, src, or clear";
      return false;
    }

    if (clear) {
      step->type = GRA_GL3_STEP_CLEAR;

      gra_gl3_step_clear_t* dst = &step->clear;

      for (size_t i = 0; i < clear->size; ++i) {
        if (HEDLEY_UNLIKELY(clear->ptr[i].type != MSGPACK_OBJECT_STR)) {
          par->error = "'clear' must be string array";
          return false;
        }
        const msgpack_object_str* v = &clear->ptr[i].via.str;

        GLenum b;
        const bool valid =
          gra_gl3_enum_unstringify_clear_bit(&b, (uint8_t*) v->ptr, v->size);
        if (HEDLEY_UNLIKELY(!valid)) {
          par->error = "'clear' has unknown target";
          return false;
        }
        dst->bits |= b;
      }

      if (HEDLEY_UNLIKELY(dstfb == NULL)) {
        par->error = "'dst' is not specified";
        return false;
      }
      const gra_gl3_pl_var_t* fb =
        gra_gl3_pl_find_var(pl, (uint8_t*) dstfb->ptr, dstfb->size);
      if (HEDLEY_UNLIKELY(fb == NULL || fb->type != GRA_GL3_PL_VAR_FB)) {
        par->error = "refers unknown framebuffer as dst";
        return false;
      }
      dst->fb = &pl->fb[fb->index];

    } else if (draw) {
      step->type = GRA_GL3_STEP_DRAW;

      const size_t unicnt    = uni?    uni->size:    0;
      const size_t shadercnt = shader? shader->size: 0;

      gra_gl3_step_draw_t* dst = &step->draw;

      if (HEDLEY_UNLIKELY(dstfb == NULL)) {
        par->error = "'dst' is not specified";
        return false;
      }
      const gra_gl3_pl_var_t* fb =
        gra_gl3_pl_find_var(pl, (uint8_t*) dstfb->ptr, dstfb->size);
      if (HEDLEY_UNLIKELY(fb == NULL || fb->type != GRA_GL3_PL_VAR_FB)) {
        par->error = "refers unknown framebuffer as dst";
        return false;
      }
      dst->fb = &pl->fb[fb->index];

      if (HEDLEY_UNLIKELY(va == NULL)) {
        par->error = "'va' is not specified";
        return false;
      }
      const gra_gl3_pl_var_t* var =
        gra_gl3_pl_find_var(pl, (uint8_t*) va->ptr, va->size);
      if (HEDLEY_UNLIKELY(var == NULL || var->type != GRA_GL3_PL_VAR_VA)) {
        par->error = "refers unknown vertex array";
        return false;
      }
      dst->va = &pl->va[var->index];

      const bool modevalid = gra_gl3_enum_unstringify_draw_mode(
        &dst->mode, (uint8_t*) draw->ptr, draw->size);
      if (HEDLEY_UNLIKELY(!modevalid)) {
        par->error = "invalid draw mode";
        return false;
      }

      if (HEDLEY_UNLIKELY(!parse_value_(par, &dst->count, count))) {
        return false;
      }
      const gra_gl3_pl_var_type_t counttype =
        gra_gl3_pl_get_value_entity_type(&dst->count);
      if (HEDLEY_UNLIKELY(counttype != GRA_GL3_PL_VAR_INTEGER)) {
        par->error = "count must be integer value";
        return false;
      }

      dst->instance = (gra_gl3_pl_value_t) {
        .type = GRA_GL3_PL_VALUE_INTEGER,
        .i    = 1,
      };
      if (instance) {
        if (HEDLEY_UNLIKELY(!parse_value_(par, &dst->instance, instance))) {
          return false;
        }
        const gra_gl3_pl_var_type_t t =
          gra_gl3_pl_get_value_entity_type(&dst->instance);
        if (HEDLEY_UNLIKELY(t != GRA_GL3_PL_VAR_INTEGER)) {
          par->error = "'instance' must be integer value";
          return false;
        }
      }

      dst->blend.mask.r = true;
      dst->blend.mask.g = true;
      dst->blend.mask.b = true;
      dst->blend.mask.a = true;
      dst->blend.eq     = GL_FUNC_ADD;
      dst->blend.src    = GL_ONE;
      dst->blend.dst    = GL_ZERO;
      if (blend) {
        const msgpack_object_map* mask = NULL;
        const msgpack_object_str* bsrc = NULL;
        const msgpack_object_str* bdst = NULL;
        const msgpack_object_str* eq   = NULL;

        const char* invalid =
          upd_msgpack_find_fields(blend, (upd_msgpack_field_t[]) {
              { .name = "mask",     .map = &mask, },
              { .name = "src",      .str = &bsrc, },
              { .name = "dst",      .str = &bdst, },
              { .name = "equation", .str = &eq,   },
              { NULL, },
            });
        if (HEDLEY_UNLIKELY(invalid)) {
          par->error = "invalid 'blend'";
          return false;
        }

        if (mask) {
          bool r = true, g = true, b = true, a = true;
          const char* invalid =
            upd_msgpack_find_fields(mask, (upd_msgpack_field_t[]) {
                { .name = "r", .b = &r, },
                { .name = "g", .b = &g, },
                { .name = "b", .b = &b, },
                { .name = "a", .b = &a, },
                { NULL, },
              });
          if (HEDLEY_UNLIKELY(invalid)) {
            par->error = "invalid 'blend.mask'";
            return false;
          }
          dst->blend.mask.r = r;
          dst->blend.mask.g = g;
          dst->blend.mask.b = b;
          dst->blend.mask.a = a;
        }

        if (eq) {
          const bool valid = gra_gl3_enum_unstringify_blend_eq(
            &dst->blend.eq, (uint8_t*) eq->ptr, eq->size);
          if (HEDLEY_UNLIKELY(!valid)) {
            par->error = "invalid 'blend.equation'";
            return false;
          }
        }

        if (bsrc) {
          const bool valid = gra_gl3_enum_unstringify_blend_factor(
            &dst->blend.src, (uint8_t*) bsrc->ptr, bsrc->size);
          if (HEDLEY_UNLIKELY(!valid)) {
            par->error = "invalid 'blend.src'";
            return false;
          }
        }

        if (bdst) {
          const bool valid = gra_gl3_enum_unstringify_blend_factor(
            &dst->blend.dst, (uint8_t*) bdst->ptr, bdst->size);
          if (HEDLEY_UNLIKELY(!valid)) {
            par->error = "invalid 'blend.dst'";
            return false;
          }
        }
      }

      dst->depth.mask = true;
      dst->depth.func = GL_LESS;
      if (depth) {
        const msgpack_object_str* func = NULL;

        const char* invalid =
          upd_msgpack_find_fields(depth, (upd_msgpack_field_t[]) {
              { .name = "mask", .b   = &dst->depth.mask },
              { .name = "func", .str = &func            },
              { NULL, },
            });
        if (HEDLEY_UNLIKELY(invalid)) {
          par->error = "invalid 'depth'";
          return false;
        }

        if (func) {
          const bool valid = gra_gl3_enum_unstringify_depth_func(
            &dst->depth.func, (uint8_t*) func->ptr, func->size);
          if (HEDLEY_UNLIKELY(!valid)) {
            par->error = "invalid 'depth.func'";
            return false;
          }
        }
      }

      if (HEDLEY_UNLIKELY(viewport == NULL)) {
        par->error = "viewport is not specified";
        return false;
      }
      if (HEDLEY_UNLIKELY(viewport->size != 2)) {
        par->error = "viewport must be an array with 2 integers";
        return false;
      }
      for (size_t j = 0; j < 2; ++j) {
        if (HEDLEY_UNLIKELY(!parse_value_(par, &dst->viewport[j], &viewport->ptr[j]))) {
          return false;
        }
        const gra_gl3_pl_var_type_t t =
          gra_gl3_pl_get_value_entity_type(&dst->viewport[j]);
        if (HEDLEY_UNLIKELY(t != GRA_GL3_PL_VAR_INTEGER)) {
          par->error = "viewport must be an array with 2 integers";
          return false;
        }
      }

      parse_stack_(par, (msgpack_object_kv*) uni - map->ptr);
      for (size_t j = 0; j < unicnt; ++j) {
        parse_stack_(par, j);

        const msgpack_object_kv* kv = &uni->ptr[j];
        if (HEDLEY_UNLIKELY(kv->key.type != MSGPACK_OBJECT_POSITIVE_INTEGER)) {
          par->error = "key must be positive integer";
          return false;
        }
        const size_t index = kv->key.via.u64;
        if (HEDLEY_UNLIKELY(index >= GRA_GL3_STEP_DRAW_UNI_MAX)) {
          par->error = "uni index is too large";
          return false;
        }
        gra_gl3_pl_value_t* v = &dst->uni[index];
        if (HEDLEY_UNLIKELY(!parse_value_(par, v, &kv->val))) {
          return false;
        }

        const gra_gl3_pl_var_type_t t = gra_gl3_pl_get_value_entity_type(v);
        const bool valid =
          t == GRA_GL3_PL_VAR_SCALAR  ||
          t == GRA_GL3_PL_VAR_INTEGER ||
          t & GRA_GL3_PL_VAR_VEC_MASK ||
          t & GRA_GL3_PL_VAR_MAT_MASK ||
          t & GRA_GL3_PL_VAR_TEX_MASK;
        if (HEDLEY_UNLIKELY(!valid)) {
          par->error = "refers var with incompatible type";
          return false;
        }

        --par->depth;
      }
      --par->depth;

      parse_stack_(par, (msgpack_object_kv*) shader - map->ptr);
      for (size_t j = 0; j < shadercnt; ++j) {
        parse_stack_(par, j);

        const msgpack_object* v = &shader->ptr[j];
        if (HEDLEY_UNLIKELY(v->type != MSGPACK_OBJECT_STR)) {
          par->error = "expected string path";
          return false;
        }
        const bool ok = parse_shfind_with_dup_(&(parse_shfind_t_) {
            .par     = par,
            .draw    = dst,
            .path    = (uint8_t*) v->via.str.ptr,
            .pathlen = v->via.str.size,
          });
        if (HEDLEY_UNLIKELY(!ok)) {
          par->error = "shader pathfind failure";
          return false;
        }
        --par->depth;
      }
      --par->depth;

    } else if (HEDLEY_UNLIKELY(srcfb)) {
      par->error = "blit is not implemented";
      return false;
    }

    --par->depth;
  }
  return true;
}

static bool parse_value_(
    parse_t_* par, gra_gl3_pl_value_t* v, const msgpack_object* obj) {
  upd_file_t*       f   = par->def;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  if (HEDLEY_UNLIKELY(obj == NULL)) {
    par->error = "empty value";
    return false;
  }

  const msgpack_object_str* str = &obj->via.str;
  switch (obj->type) {
  case MSGPACK_OBJECT_STR:
    *v = (gra_gl3_pl_value_t) {
      .type = GRA_GL3_PL_VALUE_REF,
      .var  = gra_gl3_pl_find_var(pl, (uint8_t*) str->ptr, str->size),
    };
    if (HEDLEY_UNLIKELY(v->var == NULL)) {
      par->error = "references unknown var";
      return false;
    }
    return true;

  case MSGPACK_OBJECT_FLOAT:
    *v = (gra_gl3_pl_value_t) {
      .type = GRA_GL3_PL_VALUE_SCALAR,
      .f    = obj->via.f64,
    };
    return true;

  case MSGPACK_OBJECT_POSITIVE_INTEGER:
    if (HEDLEY_UNLIKELY(obj->via.u64 > INTMAX_MAX)) {
      par->error = "integer is too large";
      return false;
    }
    *v = (gra_gl3_pl_value_t) {
      .type = GRA_GL3_PL_VALUE_INTEGER,
      .i    = (intmax_t) obj->via.u64,
    };
    return true;

  case MSGPACK_OBJECT_NEGATIVE_INTEGER:
    *v = (gra_gl3_pl_value_t) {
      .type = GRA_GL3_PL_VALUE_INTEGER,
      .i    = obj->via.i64,
    };
    return true;

  default:
    par->error = "invalid value";
    return false;
  }
}

static bool parse_shfind_with_dup_(const parse_shfind_t_* src) {
  parse_t_*   par = src->par;
  upd_file_t* f   = par->def;
  upd_iso_t*  iso = f->iso;

  parse_shfind_t_* shf = upd_iso_stack(iso, sizeof(*shf));
  if (HEDLEY_UNLIKELY(shf == NULL)) {
    return false;
  }
  *shf = *src;

  shf->pf = (upd_pathfind_t) {
    .iso   = iso,
    .path  = shf->path,
    .len   = shf->pathlen,
    .udata = shf,
    .cb    = parse_shfind_pathfind_cb_,
  };
  ++par->refcnt;
  upd_pathfind(&shf->pf);
  return true;
}


static void compile_(compile_t_* cp) {
  upd_file_t*       f   = cp->def;
  upd_iso_t*        iso = f->iso;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  assert(ctx->gl);

  cp->refcnt = 1;
  cp->ok     = true;

  for (size_t i = 0; i < pl->stepcnt; ++i) {
    if (HEDLEY_UNLIKELY(pl->step[i].type != GRA_GL3_STEP_DRAW)) {
      continue;
    }

    gra_gl3_step_draw_t* draw = &pl->step[i].draw;

    compile_draw_t_* cpd = upd_iso_stack(iso, sizeof(*cpd));
    if (HEDLEY_UNLIKELY(cpd == NULL)) {
      def_logf_(f, "compile subtask allocation failure");
      break;
    }
    *cpd = (compile_draw_t_) {
      .cp   = cp,
      .draw = draw,
      .req  = {
        .dev   = ctx->gl,
        .type  = GRA_GL3_REQ_PROG_CREATE,
        .udata = cpd,
        .cb    = compile_draw_gl_prog_create_cb_,
      },
    };
    ++cp->refcnt;
    if (HEDLEY_UNLIKELY(!gra_gl3_lock_and_req(&cpd->req))) {
      --cp->refcnt;
      def_logf_(f, "GL3_REQ_PROG_CREATE failure");
      break;
    }
  }
  compile_unref_(cp);
}

static void compile_unref_(compile_t_* cp) {
  if (HEDLEY_UNLIKELY(--cp->refcnt == 0)) {
    cp->cb(cp);
  }
}

static void compile_draw_fetch_and_attach_next_shader_(compile_draw_t_* cpd) {
  compile_t_*          cp   = cpd->cp;
  upd_file_t*          f    = cp->def;
  upd_iso_t*           iso  = f->iso;
  gra_gl3_pl_def_t*    ctx  = f->ctx;
  gra_gl3_step_draw_t* draw = cpd->draw;

  const size_t index = cpd->index;

  if (HEDLEY_UNLIKELY(index >= draw->shadercnt)) {
    cpd->req = (gra_gl3_req_t) {
      .dev   = ctx->gl,
      .type  = GRA_GL3_REQ_PROG_LINK,
      .prog  = {
        .id = draw->prog,
      },
      .udata = cpd,
      .cb    = compile_draw_gl_prog_link_cb_,
    };
    if (HEDLEY_UNLIKELY(!gra_gl3_lock_and_req(&cpd->req))) {
      def_logf_(f, "GL3_REQ_PROG_COMPILE failure");
      goto ABORT;
    }
    return;
  }

  cpd->fe = (gra_gl3_fetch_t) {
    .file  = draw->shader[index],
    .type  = GRA_GL3_FETCH_GLSL,
    .udata = cpd,
    .cb    = compile_draw_fetch_shader_cb_,
  };
  if (HEDLEY_UNLIKELY(!gra_gl3_lock_and_fetch(&cpd->fe))) {
    def_logf_(f, "GLSL fetch fetch refusal");
    goto ABORT;
  }
  return;

ABORT:
  cp->ok = false;
  upd_iso_unstack(iso, cpd);

  compile_unref_(cp);
}


static void def_init_lock_cb_(upd_file_lock_t* k) {
  upd_file_t* f   = k->udata;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    def_logf_(f, "init lock cancelled");
    goto ABORT;
  }

  const bool pf = upd_pathfind_with_dup(&(upd_pathfind_t) {
      .iso   = iso,
      .path  = (uint8_t*) GRA_GL3_DEV_PATH,
      .len   = sizeof(GRA_GL3_DEV_PATH)-1,
      .udata = k,
      .cb    = def_init_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!pf)) {
    def_logf_(f, "OpenGL device pathfind refusal");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void def_init_pathfind_cb_(upd_pathfind_t* pf) {
  upd_file_lock_t*  k   = pf->udata;
  upd_file_t*       f   = k->udata;
  upd_iso_t*        iso = f->iso;
  gra_gl3_pl_def_t* ctx = f->ctx;

  upd_file_t* gl = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(gl == NULL)) {
    def_logf_(f, "no OpenGL device found");
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(gl->driver != &gra_gl3_dev)) {
    def_logf_(f, "OpenGL device found, but it's fake");
    goto EXIT;
  }

  upd_file_ref(gl);
  ctx->gl = gl;

EXIT:
  upd_file_unlock(k);
  upd_iso_unstack(iso, k);
}

static void def_watch_obj_and_sh_cb_(upd_file_watch_t* w) {
  upd_file_t*       f   = w->udata;
  gra_gl3_pl_def_t* ctx = f->ctx;

  switch (w->event) {
  case UPD_FILE_UPDATE:
    ctx->clean = false;
    upd_file_trigger(f, UPD_FILE_UPDATE);
    break;
  }
}

static void def_parse_cb_(parse_t_* par) {
  upd_file_t* f   = par->def;
  upd_iso_t*  iso = f->iso;

  const bool parsed = par->ok;
  upd_iso_unstack(iso, par);

  if (HEDLEY_UNLIKELY(!parsed)) {
    def_logf_(f, "parse failure");
    goto ABORT;
  }

  compile_t_* cp = upd_iso_stack(iso, sizeof(*cp));
  if (HEDLEY_UNLIKELY(cp == NULL)) {
    def_logf_(f, "compile context allocation failure");
    goto ABORT;
  }
  *cp = (compile_t_) {
    .def   = f,
    .cb    = def_compile_cb_,
  };
  compile_(cp);
  return;

ABORT:
  def_finalize_fetch_(f, false);
}

static void def_compile_cb_(compile_t_* cp) {
  upd_file_t* f   = cp->def;
  upd_iso_t*  iso = f->iso;

  const bool compiled = cp->ok;
  upd_iso_unstack(iso, cp);

  if (HEDLEY_UNLIKELY(!compiled)) {
    def_logf_(f, "compile failure");
  }
  def_finalize_fetch_(f, compiled);
}

static void def_gl_prog_delete_cb_(gra_gl3_req_t* req) {
  upd_iso_t* iso = req->udata;
  upd_file_unlock(&req->lock);
  upd_iso_unstack(iso, req);
}


static void parse_lock_obj_cb_(upd_file_lock_t* k) {
  parse_t_*   par = k->udata;
  upd_file_t* f   = par->def;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    def_logf_(f, "backend file lock cancelled");
    goto ABORT;
  }
  par->locked = true;

  par->req = (upd_req_t) {
    .file  = f->backend,
    .type  = UPD_REQ_PROG_EXEC,
    .udata = par,
    .cb    = parse_exec_obj_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&par->req))) {
    def_logf_(f, "backend file exec refusal");
    goto ABORT;
  }
  return;

ABORT:
  parse_unref_(par);
}

static void parse_exec_obj_cb_(upd_req_t* req) {
  parse_t_*   par = req->udata;
  upd_file_t* f   = par->def;

  par->stream = req->prog.exec;
  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK || !par->stream)) {
    def_logf_(f, "backend file exec failure");
    goto ABORT;
  }
  upd_file_ref(par->stream);

  upd_file_unlock(&par->lock);
  par->locked = false;

  par->lock = (upd_file_lock_t) {
    .file  = par->stream,
    .ex    = true,
    .udata = par,
    .cb    = parse_lock_stream_for_write_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_lock(&par->lock))) {
    def_logf_(f, "program stream lock refusal");
    goto ABORT;
  }
  return;

ABORT:
  parse_unref_(par);
}

static void parse_lock_stream_for_write_cb_(upd_file_lock_t* k) {
  parse_t_*   par = k->udata;
  upd_file_t* f   = par->def;

  if (HEDLEY_UNLIKELY(!k->ok)) {
    def_logf_(f, "program stream lock cancelled");
    goto ABORT;
  }
  par->locked = true;

  static const uint8_t buf[] = {
    /* {"interface": "object", "command": "lock"} */
    0xDF, 0x00, 0x00, 0x00, 0x02, 0xA9, 0x69, 0x6E, 0x74, 0x65, 0x72, 0x66,
    0x61, 0x63, 0x65, 0xA6, 0x6F, 0x62, 0x6A, 0x65, 0x63, 0x74, 0xA7, 0x63,
    0x6F, 0x6D, 0x6D, 0x61, 0x6E, 0x64, 0xA4, 0x6C, 0x6F, 0x63, 0x6B,

    /* {"interface": "object", "command": "get"} */
    0xDF, 0x00, 0x00, 0x00, 0x02, 0xA9, 0x69, 0x6E, 0x74, 0x65, 0x72, 0x66,
    0x61, 0x63, 0x65, 0xA6, 0x6F, 0x62, 0x6A, 0x65, 0x63, 0x74, 0xA7, 0x63,
    0x6F, 0x6D, 0x6D, 0x61, 0x6E, 0x64, 0xA3, 0x67, 0x65, 0x74,
  };
  par->req = (upd_req_t) {
    .file = par->stream,
    .type = UPD_REQ_DSTREAM_WRITE,
    .stream = { .io = {
      .buf  = (uint8_t*) buf,
      .size = sizeof(buf),
    }, },
    .udata = par,
    .cb    = parse_write_stream_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_req(&par->req))) {
    def_logf_(f, "program stream write refusal");
    goto ABORT;
  }
  return;

ABORT:
  parse_unref_(par);
}

static void parse_write_stream_cb_(upd_req_t* req) {
  parse_t_*   par = req->udata;
  upd_file_t* f   = par->def;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    def_logf_(f, "program stream write failure");
    goto ABORT;
  }

  par->recv = (upd_msgpack_recv_t) {
    .file  = par->stream,
    .udata = par,
    .cb    = parse_recv_lock_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_msgpack_recv_init(&par->recv))) {
    def_logf_(f, "program stream receiver init failure");
    goto ABORT;
  }
  par->recv_init = true;

  upd_msgpack_recv_next(&par->recv);
  return;

ABORT:
  parse_unref_(par);
}

static void parse_recv_lock_cb_(upd_msgpack_recv_t* recv) {
  parse_t_*   par = recv->udata;
  upd_file_t* f   = par->def;

  if (HEDLEY_UNLIKELY(!recv->ok)) {
    def_logf_(f, "program stream receiver corruption");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(recv->upkd.data.type != MSGPACK_OBJECT_MAP)) {
    def_logf_(f, "backend sent invalid response");
    goto ABORT;
  }

  bool success = false;

  const char* invalid =
    upd_msgpack_find_fields(&recv->upkd.data.via.map, (upd_msgpack_field_t[]) {
        { .name = "success", .b = &success, },
        { NULL },
      });
  if (HEDLEY_UNLIKELY(invalid || !success)) {
    def_logf_(f, "program stream lock req failure");
    goto ABORT;
  }

  par->recv.cb = parse_recv_get_cb_;
  upd_msgpack_recv_next(&par->recv);
  return;

ABORT:
  parse_unref_(par);
}

static void parse_recv_get_cb_(upd_msgpack_recv_t* recv) {
  parse_t_*         par = recv->udata;
  upd_file_t*       f   = par->def;
  gra_gl3_pl_def_t* ctx = f->ctx;
  gra_gl3_pl_t*     pl  = ctx->pl;

  if (HEDLEY_UNLIKELY(!recv->ok)) {
    def_logf_(f, "program stream receiver corruption");
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(recv->upkd.data.type != MSGPACK_OBJECT_MAP)) {
    def_logf_(f, "backend sent invalid response");
    goto EXIT;
  }

  bool                      success = false;
  const msgpack_object_map* result  = NULL;

  const char* invalid =
    upd_msgpack_find_fields(&recv->upkd.data.via.map, (upd_msgpack_field_t[]) {
        { .name = "success", .b   = &success, },
        { .name = "result",  .map = &result, },
        { NULL },
      });

  if (HEDLEY_UNLIKELY(invalid || !success || !result)) {
    def_logf_(f, "program stream get req failure");
    goto EXIT;
  }

  const msgpack_object_map*   in   = NULL;
  const msgpack_object_map*   out  = NULL;
  const msgpack_object_map*   fb   = NULL;
  const msgpack_object_map*   va   = NULL;
  const msgpack_object_array* step = NULL;
  invalid = upd_msgpack_find_fields(result, (upd_msgpack_field_t[]) {
        { .name = "in",   .map   = &in,   },
        { .name = "out",  .map   = &out,  },
        { .name = "fb",   .map   = &fb,   },
        { .name = "va",   .map   = &va,   },
        { .name = "step", .array = &step, },
        { NULL },
      });
  if (HEDLEY_UNLIKELY(invalid)) {
    def_logf_(f, "program stream returned invalid object");
    goto EXIT;
  }

  const size_t incnt  = in?   in->size:   0;
  const size_t outcnt = out?  out->size:  0;
  const size_t fbcnt  = fb?   fb->size:   0;
  const size_t vacnt  = va?   va->size:   0;
  const size_t stcnt  = step? step->size: 0;

  const size_t varcnt = incnt + outcnt + fbcnt + vacnt;

  const size_t varoff = sizeof(*pl);
  const size_t outoff = varcnt*sizeof(pl->var[0]) + varoff;
  const size_t fboff  = outcnt*sizeof(pl->out[0]) + outoff;
  const size_t vaoff  = fbcnt*sizeof(pl->fb[0])   + fboff;
  const size_t stoff  = vacnt*sizeof(pl->va[0])   + vaoff;
  const size_t whole  = stcnt*sizeof(pl->step[0]) + stoff;

  def_teardown_cache_(f);
  if (HEDLEY_UNLIKELY(!upd_malloc(&pl, whole))) {
    def_logf_(f, "pipeline definition allocation failure");
    goto EXIT;
  }
  *pl = (gra_gl3_pl_t) {
    .var  = (void*) ((uint8_t*) pl + varoff),
    .out  = (void*) ((uint8_t*) pl + outoff),
    .fb   = (void*) ((uint8_t*) pl + fboff),
    .va   = (void*) ((uint8_t*) pl + vaoff),
    .step = (void*) ((uint8_t*) pl + stoff),
  };
  ctx->pl = pl;

  par->ok =
    parse_in_(par, in) &&
    parse_out_(par, out) &&
    parse_fb_(par, fb) &&
    parse_va_(par, va) &&
    parse_step_(par, step);

  if (!par->ok) {
    /* TODO: print location */
    def_logf_(f, "parse error: %s", par->error);
  }

EXIT:
  parse_unref_(par);
}


static void parse_shfind_pathfind_cb_(upd_pathfind_t* pf) {
  parse_shfind_t_*  shf = pf->udata;
  parse_t_*         par = shf->par;
  upd_file_t*       f   = par->def;
  upd_iso_t*        iso = f->iso;
  gra_gl3_pl_def_t* ctx = f->ctx;

  gra_gl3_step_draw_t* draw = shf->draw;
  assert(draw->shadercnt < GRA_GL3_STEP_DRAW_SHADER_MAX);

  upd_file_t* shader = pf->len? NULL: pf->base;
  if (HEDLEY_UNLIKELY(shader == NULL)) {
    def_logf_(f, "shader '%.*s' is not found", (int) shf->pathlen, shf->path);
    goto ABORT;
  }

  const bool valid =
    shader->driver == &gra_gl3_glsl_fragment ||
    shader->driver == &gra_gl3_glsl_vertex;
  if (HEDLEY_UNLIKELY(!valid)) {
    def_logf_(f, "shader '%.*s' is fake", (int) shf->pathlen, shf->path);
    goto ABORT;
  }

  for (size_t i = 0; i < draw->shadercnt; ++i) {
    if (HEDLEY_UNLIKELY(draw->shader[i] == shader)) {
      def_logf_(f, "shader '%.*s' is refered twice in single step",
        (int) shf->pathlen, shf->path);
      goto ABORT;
    }
  }

  upd_file_watch_t* w = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&w, sizeof(*w)))) {
    def_logf_(f,
      "shader '%.*s' watch allocation failure", (int) shf->pathlen, shf->path);
    goto ABORT;
  }
  *w = (upd_file_watch_t) {
    .file  = shader,
    .udata = f,
    .cb    = def_watch_obj_and_sh_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(w))) {
    upd_free(&w);
    def_logf_(f, "shader '%.*s' watch failure", (int) shf->pathlen, shf->path);
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&ctx->shwatch, w, SIZE_MAX))) {
    upd_file_unwatch(w);
    upd_free(&w);

    def_logf_(f,
      "shader '%.*s' watch insertion failure", (int) shf->pathlen, shf->path);
    goto ABORT;
  }

  draw->shader[draw->shadercnt++] = shader;
  upd_file_ref(shader);
  goto EXIT;

ABORT:
  par->ok = false;

EXIT:
  upd_iso_unstack(iso, shf);
  parse_unref_(par);
}


static void compile_draw_gl_prog_create_cb_(gra_gl3_req_t* req) {
  compile_draw_t_*     cpd  = req->udata;
  compile_t_*          cp   = cpd->cp;
  upd_file_t*          f    = cp->def;
  upd_iso_t*           iso  = f->iso;
  gra_gl3_step_draw_t* draw = cpd->draw;

  upd_file_unlock(&req->lock);

  if (HEDLEY_UNLIKELY(!req->ok || !req->prog.id)) {
    def_logf_(f, "OpenGL program creation failure");
    goto ABORT;
  }
  draw->prog = req->prog.id;

  cpd->index = 0;
  compile_draw_fetch_and_attach_next_shader_(cpd);
  return;

ABORT:
  cp->ok = false;
  upd_iso_unstack(iso, cpd);

  compile_unref_(cp);
}

static void compile_draw_fetch_shader_cb_(gra_gl3_fetch_t* fe) {
  compile_draw_t_*     cpd  = fe->udata;
  compile_t_*          cp   = cpd->cp;
  upd_file_t*          f    = cp->def;
  upd_iso_t*           iso  = f->iso;
  gra_gl3_pl_def_t*    ctx  = f->ctx;
  gra_gl3_step_draw_t* draw = cpd->draw;

  const size_t index = cpd->index;

  const upd_file_t* shf = draw->shader[index];
  const GLuint      sh  = fe->glsl;

  if (HEDLEY_UNLIKELY(!fe->ok || !sh)) {
    def_logf_(f, "GLSL (%s) fetch failure", shf->path);
    goto ABORT;
  }

  cpd->req = (gra_gl3_req_t) {
    .dev = ctx->gl,
    .type = GRA_GL3_REQ_PROG_ATTACH,
    .prog = {
      .id     = draw->prog,
      .shader = sh,
    },
    .udata = cpd,
    .cb    = compile_draw_gl_prog_attach_cb_,
  };
  if (HEDLEY_UNLIKELY(!gra_gl3_lock_and_req(&cpd->req))) {
    def_logf_(f, "GL3_REQ_PROG_ATTACH failure");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(&cpd->fe.lock);
  upd_iso_unstack(iso, cpd);

  cp->ok = false;
  compile_unref_(cp);
}

static void compile_draw_gl_prog_attach_cb_(gra_gl3_req_t* req) {
  compile_draw_t_* cpd = req->udata;
  compile_t_*      cp  = cpd->cp;
  upd_file_t*      f   = cp->def;
  upd_iso_t*       iso = f->iso;

  upd_file_unlock(&cpd->req.lock);
  upd_file_unlock(&cpd->fe.lock);

  if (HEDLEY_UNLIKELY(!req->ok)) {
    def_logf_(f, "failed to attach OpenGL shader to OpenGL program");
    goto ABORT;
  }

  ++cpd->index;
  compile_draw_fetch_and_attach_next_shader_(cpd);
  return;

ABORT:
  upd_file_unlock(&cpd->fe.lock);
  upd_iso_unstack(iso, cpd);

  cp->ok = false;
  compile_unref_(cp);
}

static void compile_draw_gl_prog_link_cb_(gra_gl3_req_t* req) {
  compile_draw_t_* cpd = req->udata;
  compile_t_*      cp  = cpd->cp;
  upd_file_t*      f   = cp->def;
  upd_iso_t*       iso = f->iso;

  upd_file_unlock(&req->lock);

  if (HEDLEY_UNLIKELY(!req->ok)) {
    def_logf_(f, "OpenGL program link failure");
    cp->ok = false;
  }
  upd_iso_unstack(iso, cpd);
  compile_unref_(cp);
}
