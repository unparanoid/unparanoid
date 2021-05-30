#include "common.h"


#define MAX_DIM_  8
#define MAX_RANK_ 4


typedef struct tensor_t_ {
  upd_file_t*      file;
  upd_file_watch_t watch;

  upd_req_tensor_meta_t meta;

  uint8_t* data;
  size_t   size;

  upd_tensor_type_t type[MAX_DIM_];
  uint32_t          reso[MAX_RANK_];
} tensor_t_;


static
bool
tensor_init_(
  upd_file_t* f);

static
void
tensor_deinit_(
  upd_file_t* f);

static
bool
tensor_handle_(
  upd_req_t* req);


const upd_driver_t upd_driver_tensor = {
  .name = (uint8_t*) "upd.tensor",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_TENSOR,
    0,
  },
  .init   = tensor_init_,
  .deinit = tensor_deinit_,
  .handle = tensor_handle_,
};


static bool tensor_init_(upd_file_t* f) {
  tensor_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (tensor_t_) {
    .file = f,
  };
  f->ctx = ctx;
  return true;
}

static void tensor_deinit_(upd_file_t* f) {
  tensor_t_* ctx = f->ctx;

  upd_free(&ctx->data);
  upd_free(&ctx);
}

static bool tensor_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  tensor_t_*  ctx = f->ctx;

  switch (req->type) {
  case UPD_REQ_TENSOR_ACCESS:
    req->tensor.access = (upd_req_tensor_access_t) {
      .alloc = true,
      .meta  = true,
      .data  = true,
    };
    break;

  case UPD_REQ_TENSOR_ALLOC: {
    const upd_req_tensor_meta_t* m   = &req->tensor.meta;
    const size_t                 dim = m->rank? m->reso[0]: 1;
    if (HEDLEY_UNLIKELY(m->rank >= MAX_RANK_ || dim >= MAX_DIM_)) {
      req->result = UPD_REQ_INVALID;
      return false;
    }

    size_t n = 0;
    for (size_t i = 0; i < dim; ++i) {
      n += upd_tensor_type_sizeof(m->type[i]);
    }
    for (size_t i = 1; i < m->rank; ++i) {
      n *= m->reso[i];
    }

    if (HEDLEY_UNLIKELY(!upd_malloc(&ctx->data, n))) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    ctx->size = n;

    memcpy(ctx->type, m->type, sizeof(*m->type)*dim);
    memcpy(ctx->reso, m->reso, sizeof(*m->reso)*m->rank);

    ctx->meta = (upd_req_tensor_meta_t) {
      .rank    = m->rank,
      .type    = ctx->type,
      .reso    = ctx->reso,
      .inplace = true,
    };
  } break;

  case UPD_REQ_TENSOR_META:
    req->tensor.meta = ctx->meta;
    break;

  case UPD_REQ_TENSOR_DATA:
    req->tensor.data = (upd_req_tensor_data_t) {
      .meta = ctx->meta,
      .ptr  = ctx->data,
      .size = ctx->size,
    };
    break;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
  req->result = UPD_REQ_OK;
  req->cb(req);
  return true;
}
