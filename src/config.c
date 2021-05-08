#include "common.h"


#define CONFIG_FILE_     "upd.yml"

#define CONFIG_FILE_MAX_ (1024*1024*4)  /* = 4 MiB */


typedef struct server_pathfind_t_ {
  upd_req_pathfind_t super;

  upd_config_apply_t* ap;
  yaml_node_t*        node;

  const uint8_t* path;
  size_t         len;
  uint16_t       port;
} server_pathfind_t_;


static
void
config_unref_(
  upd_config_apply_t* ap);

static
void
config_parse_(
  upd_config_apply_t* ap);

static
void
config_parse_server_(
  upd_config_apply_t* ap,
  yaml_node_t*        node);


static
void
config_parse_server_pathfind_cb_(
  upd_req_pathfind_t* pf);


static
void
config_stat_cb_(
  uv_fs_t* req);

static
void
config_open_cb_(
  uv_fs_t* req);

static
void
config_read_cb_(
  uv_fs_t* req);

static
void
config_close_cb_(
  uv_fs_t* req);


void upd_config_apply(upd_config_apply_t* ap) {
  ap->ok = false;

  uint8_t temp[UPD_PATH_MAX];
  utf8cpy(temp, ap->path);

  const size_t len = cwk_path_join(
    (char*) temp, CONFIG_FILE_, (char*) ap->path, UPD_PATH_MAX);
  if (HEDLEY_UNLIKELY(len >= UPD_PATH_MAX)) {
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "config path is too long: %s", ap->path);
    return;
  }

  const bool stat = 0 <= uv_fs_stat(
    &ap->iso->loop, &ap->fs, (char*) ap->path, config_stat_cb_);
  if (HEDLEY_UNLIKELY(!stat)) {
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "stat failure: %s", ap->path);
    return;
  }
}


static void config_unref_(upd_config_apply_t* ap) {
  if (HEDLEY_UNLIKELY(--ap->refcnt == 0)) {
    yaml_document_delete(&ap->doc);
    ap->ok = true;
    ap->cb(ap);
  }
}

static void config_parse_(upd_config_apply_t* ap) {
  static const struct {
    const char* name;
    void
    (*func)(
      upd_config_apply_t* ap,
      yaml_node_t*        node);
  } subparsers[] = {
    { "server", config_parse_server_, },
  };

  yaml_node_t* root = yaml_document_get_root_node(&ap->doc);
  if (HEDLEY_UNLIKELY(root->type != YAML_MAPPING_NODE)) {
    upd_iso_msgf(ap->iso, "yaml root is not mapping: %s\n", ap->path);
    return;
  }
  yaml_node_pair_t* itr = root->data.mapping.pairs.start;
  yaml_node_pair_t* end = root->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(&ap->doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(&ap->doc, itr->value);
    if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
      upd_iso_msgf(ap->iso, "yaml fatal error: %s\n", ap->path);
      continue;
    }
    const uint8_t* name    = key->data.scalar.value;
    const size_t   namelen = key->data.scalar.length;

    bool handled = false;
    for (size_t i = 0; i < sizeof(subparsers)/sizeof(subparsers); ++i) {
      const bool match =
        utf8ncmp(subparsers[i].name, name, namelen) == 0 &&
        utf8size_lazy(subparsers[i].name) == namelen;
      if (HEDLEY_UNLIKELY(match)) {
        subparsers[i].func(ap, val);
        handled = true;
        break;
      }
    }
    if (HEDLEY_UNLIKELY(!handled)) {
      upd_iso_msgf(ap->iso, "unknown block '%.*s': %s (%zu:%zu)\n",
        (int) namelen, name,
        ap->path, key->start_mark.line, key->start_mark.column);
      continue;
    }
  }
}

static void config_parse_server_(upd_config_apply_t* ap, yaml_node_t* node) {
  if (HEDLEY_UNLIKELY(!node)) {
    return;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_MAPPING_NODE)) {
    upd_iso_msgf(ap->iso,
      "invalid server specification: %s (%zu:%zu)\n",
      ap->path, node->start_mark.line, node->start_mark.column);
    return;
  }

  yaml_node_pair_t* itr = node->data.mapping.pairs.start;
  yaml_node_pair_t* end = node->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(&ap->doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(&ap->doc, itr->value);
    if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
      upd_iso_msgf(ap->iso,
        "yaml fatal error: %s (%zu:%zu)\n",
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }

    const uint8_t* name    = key->data.scalar.value;
    const size_t   namelen = key->data.scalar.length;

    if (HEDLEY_UNLIKELY(!val || val->type != YAML_SCALAR_NODE)) {
      upd_iso_msgf(ap->iso,
        "scalar expected for value of '%.*s': %s (%zu:%zu)\n",
        (int) namelen, name,
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }

    const uint8_t* value    = val->data.scalar.value;
    const size_t   valuelen = val->data.scalar.length;

    char* temp;
    const uintmax_t port = strtoumax((char*) value, &temp, 0);
    if (HEDLEY_UNLIKELY(*temp != 0 || port == 0 || port > UINT16_MAX)) {
      upd_iso_msgf(ap->iso,
        "invalid port specification '%.*s': %s (%zu:%zu)\n",
        (int) valuelen, value,
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }

    server_pathfind_t_* pf = upd_iso_stack(ap->iso, sizeof(*pf));
    if (HEDLEY_UNLIKELY(pf == NULL)) {
      upd_iso_msgf(ap->iso, "server pathfind context allocation failure\n");
      continue;
    }
    *pf = (server_pathfind_t_) {
      .super = {
        .iso  = ap->iso,
        .path = (uint8_t*) name,
        .len  = namelen,
        .cb   = config_parse_server_pathfind_cb_,
      },
      .ap   = ap,
      .node = key,
      .path = name,
      .len  = namelen,
      .port = port,
    };
    ++ap->refcnt;
    upd_req_pathfind(&pf->super);
  }
}


static void config_parse_server_pathfind_cb_(upd_req_pathfind_t* pf_) {
  server_pathfind_t_* pf = (void*) pf_;
  upd_config_apply_t* ap = pf->ap;

  yaml_node_t*   node = pf->node;
  const uint16_t port = pf->port;
  const uint8_t* path = pf->path;
  const size_t   len  = pf->len;

  const bool found = !pf->super.len;
  upd_iso_unstack(ap->iso, pf);

  if (HEDLEY_UNLIKELY(!found)) {
    upd_iso_msgf(ap->iso, "unknown program '%.*s': %s (%zu:%zu)\n",
      (int) len, path,
      ap->path, node->start_mark.line, node->start_mark.column);
    goto EXIT;
  }

  const bool srv = upd_srv_new_tcp(
    pf->ap->iso, pf->super.base, (uint8_t*) "0.0.0.0", pf->port);
  if (HEDLEY_UNLIKELY(!srv)) {
    upd_iso_msgf(pf->ap->iso,
      "failed to start server on tcp %"PRIu16": %s (%zu:%zu)",
      port,
      ap->path, node->start_mark.line, node->start_mark.column);
    goto EXIT;
  }

EXIT:
  config_unref_(pf->ap);
}


static void config_stat_cb_(uv_fs_t* req) {
  upd_config_apply_t* ap = (void*) req;

  const ssize_t result = req->result;
  uv_fs_req_cleanup(req);

  if (HEDLEY_UNLIKELY(result != 0)) {
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "stat failure: %s\n", ap->path);
    return;
  }

  const bool open = 0 <= uv_fs_open(
    &ap->iso->loop, &ap->fs, (char*) ap->path, 0, O_RDONLY, config_open_cb_);
  if (HEDLEY_UNLIKELY(!open)) {
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "open failure: %s\n", ap->path);
    return;
  }
}

static void config_open_cb_(uv_fs_t* req) {
  upd_config_apply_t* ap = (void*) req;

  const ssize_t result = req->result;
  const size_t  size   = req->statbuf.st_size;
  uv_fs_req_cleanup(req);

  if (HEDLEY_UNLIKELY(result < 0)) {
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "open failure: %s\n", ap->path);
    return;
  }
  ap->fd = result;

  if (HEDLEY_UNLIKELY(size > CONFIG_FILE_MAX_)) {
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "too large config: %s\n", ap->path);
    return;
  }

  ap->buf = upd_iso_stack(ap->iso, size);
  if (HEDLEY_UNLIKELY(ap->buf == NULL)) {
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "config file buffer allocation failure: %s\n", ap->path);
    return;
  }

  const uv_buf_t buf = uv_buf_init((char*) ap->buf, size);

  const bool read = 0 <= uv_fs_read(
    &ap->iso->loop, &ap->fs, ap->fd, &buf, 1, 0, config_read_cb_);
  if (HEDLEY_UNLIKELY(!read)) {
    upd_iso_unstack(ap->iso, ap->buf);
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "read failure: %s\n", ap->path);
    return;
  }
}

static void config_read_cb_(uv_fs_t* req) {
  upd_config_apply_t* ap = (void*) req;

  const ssize_t result = req->result;
  uv_fs_req_cleanup(req);

  if (HEDLEY_UNLIKELY(result < 0)) {
    upd_iso_unstack(ap->iso, ap->buf);
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "read failure: %s\n", ap->path);
    return;
  }

  yaml_parser_t parser = {0};
  if (HEDLEY_UNLIKELY(!yaml_parser_initialize(&parser))) {
    upd_iso_unstack(ap->iso, ap->buf);
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "yaml parser allocation failure\n");
    return;
  }
  yaml_parser_set_input_string(&parser, ap->buf, result);

  const bool parse = yaml_parser_load(&parser, &ap->doc);
  yaml_parser_delete(&parser);
  upd_iso_unstack(ap->iso, ap->buf);
  if (HEDLEY_UNLIKELY(!parse)) {
    ap->cb(ap);
    upd_iso_msgf(ap->iso, "yaml error: %s\n", ap->path);
    return;
  }

  ap->refcnt = 1;
  config_parse_(ap);

  const bool close = 0 <= uv_fs_close(
    &ap->iso->loop, &ap->fs, ap->fd, config_close_cb_);
  if (HEDLEY_UNLIKELY(!close)) {
    config_unref_(ap);
    upd_iso_msgf(ap->iso, "close failure: %s\n", ap->path);
    return;
  }
}

static void config_close_cb_(uv_fs_t* req) {
  upd_config_apply_t* ap = (void*) req;

  const ssize_t result = req->result;
  uv_fs_req_cleanup(req);

  if (HEDLEY_UNLIKELY(result != 0)) {
    upd_iso_msgf(ap->iso, "close failure: %s\n", ap->path);
  }
  config_unref_(ap);
}
