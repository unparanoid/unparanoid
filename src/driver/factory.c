#include "common.h"


#define LOG_PREFIX_ "upd.factory: "


typedef struct ctx_t_ {
  const upd_driver_t* driver;

  uint8_t* param;
  size_t   paramlen;

  unsigned inherit : 1;
} ctx_t_;


static
bool
factory_parse_param_(
  upd_iso_t*       iso,
  ctx_t_*          proto,
  yaml_document_t* doc);

static
bool
factory_init_(
  upd_file_t* f);

static
void
factory_deinit_(
  upd_file_t* f);

static
bool
factory_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_factory = {
  .name = (uint8_t*) "upd.factory",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_PROG,
    0,
  },
  .init   = factory_init_,
  .deinit = factory_deinit_,
  .handle = factory_handle_,
};


static bool factory_parse_param_(
    upd_iso_t* iso, ctx_t_* ctx, yaml_document_t* doc) {
  const yaml_node_t* node = yaml_document_get_root_node(doc);

  const yaml_node_t* driver = NULL;
  const yaml_node_t* param  = NULL;

  bool inherit = true;

  switch (node->type) {
  case YAML_SCALAR_NODE:
    driver = node;
    break;

  case YAML_MAPPING_NODE: {
    const char* invalid =
      upd_yaml_find_fields(doc, node, (upd_yaml_field_t[]) {
          { .name = "driver",  .required = true,  .str = &driver,  },
          { .name = "param",   .required = false, .str = &param,   },
          { .name = "inherit", .required = false, .b   = &inherit, },
          { NULL },
        });
    if (HEDLEY_UNLIKELY(invalid)) {
      upd_iso_msgf(iso, LOG_PREFIX_"invalid param field: %s\n", invalid);
      return false;
    }
  } break;

  default:
    return false;
  }

  const uint8_t* dname    = driver->data.scalar.value;
  const size_t   dnamelen = driver->data.scalar.length;
  const upd_driver_t* d = upd_driver_lookup(iso, dname, dnamelen);
  if (HEDLEY_UNLIKELY(d == NULL)) {
    upd_iso_msgf(
      iso, LOG_PREFIX_"unknown driver: %.*s\n", (int) dnamelen, dname);
    return false;
  }

  *ctx = (ctx_t_) {
    .driver = d,
  };
  if (param) {
    ctx->param    = param->data.scalar.value;
    ctx->paramlen = param->data.scalar.length;
  }
  return true;
}

static bool factory_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  ctx_t_ proto = {0};

  yaml_document_t doc = {0};
  if (HEDLEY_UNLIKELY(!upd_yaml_parse(&doc, f->param, f->paramlen))) {
    upd_iso_msgf(iso, LOG_PREFIX_"param must satisfy yaml format\n");
    return false;
  }
  if (HEDLEY_UNLIKELY(!factory_parse_param_(iso, &proto, &doc))) {
    yaml_document_delete(&doc);
    upd_iso_msgf(iso, LOG_PREFIX_"param parse failure\n");
    return false;
  }

  const size_t whole = sizeof(proto) + proto.paramlen;

  ctx_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, whole))) {
    yaml_document_delete(&doc);
    upd_iso_msgf(iso, LOG_PREFIX_"context allocation failure\n");
    return false;
  }
  *ctx = proto;

  ctx->param = utf8ncpy(ctx+1, proto.param, ctx->paramlen);
  yaml_document_delete(&doc);

  f->ctx = ctx;
  return true;
}

static void factory_deinit_(upd_file_t* f) {
  ctx_t_* ctx = f->ctx;
  upd_free(&ctx);
}

static bool factory_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  upd_iso_t*  iso = f->iso;
  ctx_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(req->type != UPD_REQ_PROG_EXEC)) {
    req->result = UPD_REQ_INVALID;
    return false;
  }

  upd_file_t proto = {
    .iso    = iso,
    .driver = ctx->driver,
  };
  if (ctx->inherit) {
    proto.path     = f->path;
    proto.pathlen  = f->pathlen;
    proto.npath    = f->npath;
    proto.npathlen = f->npathlen;
  }

  upd_file_t* prod = upd_file_new(&proto);
  if (HEDLEY_UNLIKELY(prod == NULL)) {
    req->result = UPD_REQ_ABORTED;
    upd_iso_msgf(iso, LOG_PREFIX_"product creation failure\n");
    return false;
  }

  req->prog.exec = prod;
  req->result    = UPD_REQ_OK;
  req->cb(req);
  upd_file_unref(prod);
  return true;
}
