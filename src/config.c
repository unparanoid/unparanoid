#include "common.h"


#define CONFIG_FILE_     "upd.yml"

#define CONFIG_FILE_MAX_ (1024*1024*4)  /* = 4 MiB */


typedef struct task_t_ {
  upd_config_apply_t* ap;
  yaml_node_t*        node;

  union {
    struct {
      const uint8_t* upath;
      size_t         ulen;
      uint16_t       port;
    } server;

    struct {
      const uint8_t* upath;
      size_t         ulen;
      size_t         uoffset;

      const uint8_t* npath;
      size_t         nlen;

      yaml_node_t* rules;
    } sync;
  };
} task_t_;


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
config_parse_sync_(
  upd_config_apply_t* ap,
  yaml_node_t*        node);


static
void
config_parse_server_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
config_parse_sync_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
config_parse_sync_lock_for_add_cb_(
  upd_file_lock_t* lock);

static
void
config_parse_sync_add_cb_(
  upd_req_t* req);


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
    { "sync",   config_parse_sync_,   },
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
    for (size_t i = 0; i < sizeof(subparsers)/sizeof(subparsers[0]); ++i) {
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

    task_t_* task = upd_iso_stack(ap->iso, sizeof(*task));
    if (HEDLEY_UNLIKELY(task == NULL)) {
      upd_iso_msgf(ap->iso,
        "task allocation failure: %s (%zu:%zu)\n",
        ap->path, node->start_mark.line, node->start_mark.column);
      break;
    }
    *task = (task_t_) {
      .ap   = ap,
      .node = key,
      .server = {
        .upath = name,
        .ulen  = namelen,
        .port  = port,
      },
    };

    ++ap->refcnt;
    const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
        .iso   = ap->iso,
        .path  = (uint8_t*) name,
        .len   = namelen,
        .udata = task,
        .cb    = config_parse_server_pathfind_cb_,
      });
    if (HEDLEY_UNLIKELY(!pf)) {
      upd_iso_unstack(ap->iso, task);
      config_unref_(ap);
      upd_iso_msgf(ap->iso,
        "pathfind failure: %s (%zu:%zu)\n",
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }
  }
}

static void config_parse_sync_(upd_config_apply_t* ap, yaml_node_t* node) {
  if (HEDLEY_UNLIKELY(!node)) {
    return;
  }
  if (HEDLEY_UNLIKELY(node->type != YAML_MAPPING_NODE)) {
    upd_iso_msgf(ap->iso,
      "invalid sync specification: %s (%zu:%zu)\n",
      ap->path, node->start_mark.line, node->start_mark.column);
    return;
  }

  yaml_node_pair_t* itr = node->data.mapping.pairs.start;
  yaml_node_pair_t* end = node->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(&ap->doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(&ap->doc, itr->value);
    if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
      upd_iso_msgf(ap->iso, "yaml fatal error: %s (%zu:%zu)\n",
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }

    const uint8_t* name    = key->data.scalar.value;
    const size_t   namelen = key->data.scalar.length;
    const size_t   dirlen  = upd_path_dirname((uint8_t*) name, namelen);

    if (HEDLEY_UNLIKELY(!val || val->type != YAML_MAPPING_NODE)) {
      upd_iso_msgf(ap->iso,
        "mapping expected for value of '%.*s': %s (%zu:%zu)\n",
        (int) namelen, name,
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }

    task_t_* task = upd_iso_stack(ap->iso, sizeof(*task));
    if (HEDLEY_UNLIKELY(task == NULL)) {
      upd_iso_msgf(ap->iso,
        "task allocation failure: %s (%zu:%zu)\n",
        ap->path, node->start_mark.line, node->start_mark.column);
      break;
    }
    *task = (task_t_) {
      .ap   = ap,
      .node = key,
      .sync = {
        .upath   = name,
        .ulen    = namelen,
        .uoffset = dirlen,
      },
    };

    yaml_node_pair_t* itr = val->data.mapping.pairs.start;
    yaml_node_pair_t* end = val->data.mapping.pairs.top;
    for (; itr < end; ++itr) {
      yaml_node_t* key = yaml_document_get_node(&ap->doc, itr->key);
      yaml_node_t* val = yaml_document_get_node(&ap->doc, itr->value);
      if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
        upd_iso_msgf(ap->iso, "yaml fatal error: %s (%zu:%zu)\n",
          ap->path, node->start_mark.line, node->start_mark.column);
        continue;
      }

      const uint8_t* name    = key->data.scalar.value;
      const size_t   namelen = key->data.scalar.length;

#     define nameq_(v) (utf8ncmp(name, v, namelen) == 0 && v[namelen] == 0)

      if (nameq_("npath")) {
        if (HEDLEY_UNLIKELY(!val || val->type != YAML_SCALAR_NODE)) {
          upd_iso_msgf(ap->iso,
            "scalar expected for 'npath' field: %s (%zu:%zu)\n",
            ap->path, node->start_mark.line, node->start_mark.column);
          continue;
        }
        task->sync.npath = val->data.scalar.value;
        task->sync.nlen  = val->data.scalar.length;

      } else if (nameq_("rules")) {
        if (HEDLEY_UNLIKELY(!val || val->type != YAML_MAPPING_NODE)) {
          upd_iso_msgf(ap->iso,
            "mapping expected for 'npath' field: %s (%zu:%zu)\n",
            ap->path, node->start_mark.line, node->start_mark.column);
          continue;
        }
        task->sync.rules = val;

      } else {
        upd_iso_msgf(ap->iso, "unknown field '%.*s': %s (%zu:%zu)\n",
          (int) namelen, name,
          ap->path, node->start_mark.line, node->start_mark.column);
        continue;
      }

#     undef nameq_
    }

    if (HEDLEY_UNLIKELY(task->sync.nlen == 0 || task->sync.rules == NULL)) {
      upd_iso_unstack(ap->iso, task);
      upd_iso_msgf(ap->iso,
        "requires 'npath' and 'rules' field: %s (%zu:%zu)\n",
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }

    ++ap->refcnt;
    const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
        .iso    = ap->iso,
        .path   = (uint8_t*) name,
        .len    = dirlen,
        .create = true,
        .udata  = task,
        .cb     = config_parse_sync_pathfind_cb_,
      });
    if (HEDLEY_UNLIKELY(!pf)) {
      upd_iso_unstack(ap->iso, task);
      config_unref_(ap);
      upd_iso_msgf(ap->iso,
        "pathfind failure: %s (%zu:%zu)\n",
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }
  }
}


static void config_parse_server_pathfind_cb_(upd_req_pathfind_t* pf) {
  task_t_*            task = pf->udata;
  upd_config_apply_t* ap   = task->ap;

  yaml_node_t*   node  = task->node;
  const uint16_t port  = task->server.port;
  const uint8_t* upath = task->server.upath;
  const size_t   ulen  = task->server.ulen;

  upd_file_t* file = pf->len? NULL: pf->base;
  upd_iso_unstack(ap->iso, pf);
  upd_iso_unstack(ap->iso, task);

  if (HEDLEY_UNLIKELY(!file)) {
    upd_iso_msgf(ap->iso, "unknown program '%.*s': %s (%zu:%zu)\n",
      (int) ulen, upath,
      ap->path, node->start_mark.line, node->start_mark.column);
    goto EXIT;
  }

  const bool srv = upd_srv_new_tcp(ap->iso, file, (uint8_t*) "0.0.0.0", port);
  if (HEDLEY_UNLIKELY(!srv)) {
    upd_iso_msgf(ap->iso,
      "failed to start server on tcp %"PRIu16": %s (%zu:%zu)",
      port,
      ap->path, node->start_mark.line, node->start_mark.column);
    goto EXIT;
  }

EXIT:
  config_unref_(ap);
}

static void config_parse_sync_pathfind_cb_(upd_req_pathfind_t* pf) {
  task_t_*            task = pf->udata;
  upd_config_apply_t* ap   = task->ap;
  yaml_node_t*        node = task->node;

  upd_file_t* base = pf->len? NULL: pf->base;
  upd_iso_unstack(ap->iso, pf);

  if (HEDLEY_UNLIKELY(!base)) {
    upd_iso_msgf(ap->iso, "parent dir creation failure: %s (%zu:%zu)\n",
      ap->path, node->start_mark.line, node->start_mark.column);
    goto ABORT;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = base,
      .ex    = true,
      .udata = task,
      .cb    = config_parse_sync_lock_for_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_iso_msgf(ap->iso, "parent dir lock failure: %s (%zu:%zu)\n",
      ap->path, node->start_mark.line, node->start_mark.column);
    goto ABORT;
  }
  return;

ABORT:
  upd_iso_unstack(ap->iso, task);
  config_unref_(ap);
}

static void config_parse_sync_lock_for_add_cb_(upd_file_lock_t* lock) {
  task_t_*            task = lock->udata;
  upd_config_apply_t* ap   = task->ap;
  yaml_node_t*        node = task->node;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    upd_iso_msgf(ap->iso, "lock failure: %s (%zu:%zu)\n",
      ap->path, node->start_mark.line, node->start_mark.column);
    goto ABORT;
  }

  upd_file_t* f = upd_file_new_from_npath(
    ap->iso, &upd_driver_syncdir, task->sync.npath, task->sync.nlen);
  if (HEDLEY_UNLIKELY(!f)) {
    upd_iso_msgf(ap->iso, "syncdir file creation failure: %s (%zu:%zu)\n",
      ap->path, node->start_mark.line, node->start_mark.column);
    goto ABORT;
  }

  upd_array_of(upd_driver_rule_t*) rules = {0};

  yaml_node_pair_t* itr = task->sync.rules->data.mapping.pairs.start;
  yaml_node_pair_t* end = task->sync.rules->data.mapping.pairs.top;
  for (; itr < end; ++itr) {
    yaml_node_t* key = yaml_document_get_node(&ap->doc, itr->key);
    yaml_node_t* val = yaml_document_get_node(&ap->doc, itr->value);

    if (HEDLEY_UNLIKELY(!key || key->type != YAML_SCALAR_NODE)) {
      upd_iso_msgf(ap->iso, "yaml fatal error: %s (%zu:%zu)\n",
        ap->path, node->start_mark.line, node->start_mark.column);
      continue;
    }
    if (HEDLEY_UNLIKELY(!val || val->type != YAML_SCALAR_NODE)) {
      upd_iso_msgf(ap->iso,
        "scalar expected for driver name: %s (%zu:%zu)\n",
        ap->path, key->start_mark.line, key->start_mark.column);
      continue;
    }

    const upd_driver_t* driver = upd_driver_lookup(
      ap->iso, val->data.scalar.value, val->data.scalar.length);
    if (HEDLEY_UNLIKELY(driver == NULL)) {
      upd_iso_msgf(ap->iso,
        "unknown driver '%.*s': %s (%zu:%zu)\n",
        (int) val->data.scalar.length, val->data.scalar.value,
        ap->path, key->start_mark.line, key->start_mark.column);
      continue;
    }

    const size_t tail = key->data.scalar.length + 1;

    upd_driver_rule_t* r = NULL;
    if (HEDLEY_UNLIKELY(!upd_malloc(&r, sizeof(*r)+tail))) {
      upd_iso_msgf(ap->iso,
        "driver rule allocation failure: %s (%zu:%zu)\n",
        ap->path, key->start_mark.line, key->start_mark.column);
      break;
    }
    *r = (upd_driver_rule_t) {
      .ext    = (uint8_t*) (r+1),
      .len    = key->data.scalar.length,
      .driver = driver,
    };
    utf8ncpy(r->ext, key->data.scalar.value, r->len);
    r->ext[r->len] = 0;

    if (HEDLEY_UNLIKELY(!upd_array_insert(&rules, r, SIZE_MAX))) {
      upd_free(&r);
      upd_iso_msgf(ap->iso,
        "driver rule insertion failure: %s (%zu:%zu)\n",
        ap->path, key->start_mark.line, key->start_mark.column);
      break;
    }
  }
  upd_driver_syncdir_set_rules(f, &rules);  /* ownership moves */

  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file = lock->file,
      .type = UPD_REQ_DIR_ADD,
      .dir  = { .entry = {
        .file = f,
        .name = (uint8_t*) (task->sync.upath + task->sync.uoffset),
        .len  = task->sync.ulen - task->sync.uoffset,
      }, },
      .udata = lock,
      .cb    = config_parse_sync_add_cb_,
    });
  upd_file_unref(f);

  if (HEDLEY_UNLIKELY(!add)) {
    upd_iso_msgf(ap->iso, "add request refused: %s (%zu:%zu)\n",
      ap->path, node->start_mark.line, node->start_mark.column);
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(ap->iso, lock);

  upd_iso_unstack(ap->iso, task);

  config_unref_(ap);
}

static void config_parse_sync_add_cb_(upd_req_t* req) {
  upd_file_lock_t*    lock = req->udata;
  task_t_*            task = lock->udata;
  upd_config_apply_t* ap   = task->ap;
  yaml_node_t*        node = task->node;

  const bool added = req->dir.entry.file;
  upd_iso_unstack(ap->iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(ap->iso, lock);

  if (HEDLEY_UNLIKELY(!added)) {
    upd_iso_msgf(ap->iso, "add request failure: %s (%zu:%zu)\n",
      ap->path, node->start_mark.line, node->start_mark.column);
    goto EXIT;
  }

EXIT:
  upd_iso_unstack(ap->iso, task);
  config_unref_(ap);
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
