#include "common.h"

#define SRV_PATH_    "/var/srv"
#define TCP_BACKLOG_ 256


HEDLEY_PRINTF_FORMAT(2, 3)
static
void
srv_logf_(
  upd_srv_t*  srv,
  const char* fmt,
  ...);


static
void
srv_lock_for_access_cb_(
  upd_file_lock_t* lock);

static
void
srv_access_cb_(
  upd_req_t* req);

static
void
srv_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
srv_lock_dir_cb_(
  upd_file_lock_t* l);

static
void
srv_add_cb_(
  upd_req_t* req);

static
void
srv_conn_cb_(
  uv_stream_t* stream,
  int          status);

static
void
srv_close_cb_(
  uv_handle_t* handle);


upd_srv_t* upd_srv_new_tcp(
    upd_iso_t* iso, upd_file_t* prog, const uint8_t* addr, uint16_t port) {
  struct sockaddr_in a = {0};
  if (HEDLEY_UNLIKELY(0 > uv_ip4_addr((char*) addr, port, &a))) {
    upd_iso_msgf(iso, "srv error: invalid address '%s:%"PRIu16"'\n", addr, port);
    return NULL;
  }

  char name[32] = {0};
  const size_t namelen = snprintf(name, sizeof(name), "tcp-%"PRIu16, port);

  upd_srv_t* srv = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&srv, sizeof(*srv)+namelen+1))) {
    upd_iso_msgf(iso, "srv error: allocation failure\n");
    return NULL;
  }
  *srv = (upd_srv_t) {
    .iso  = iso,
    .prog = prog,
    .name = (uint8_t*) (srv+1),
  };
  utf8ncpy(srv->name, name, namelen);
  srv->name[namelen] = 0;

  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&iso->loop, &srv->uv.tcp))) {
    srv_logf_(srv, "tcp server allocation failure");
    upd_free(&srv);
    return NULL;
  }

  upd_file_ref(srv->prog);

  if (HEDLEY_UNLIKELY(0 > uv_tcp_bind(&srv->uv.tcp, (struct sockaddr*) &a, 0))) {
    srv_logf_(srv, "tcp bind failure (%s:%"PRIu16")", addr, port);
    upd_srv_delete(srv);
    return NULL;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = prog,
      .udata = srv,
      .cb    = srv_lock_for_access_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    srv_logf_(srv, "failed to allocate server program lock for access req");
    upd_srv_delete(srv);
    return NULL;
  }
  return srv;
}

void upd_srv_delete(upd_srv_t* srv) {
  upd_array_find_and_remove(&srv->iso->srv, srv);

  if (HEDLEY_LIKELY(srv->dir)) {
    upd_file_unref(srv->dir);
  }
  upd_file_unref(srv->prog);

  uv_close(&srv->uv.handle, srv_close_cb_);
}


static void srv_logf_(upd_srv_t* srv, const char* fmt, ...) {
  upd_iso_msgf(srv->iso, "srv error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(srv->iso, fmt, args);
  va_end(args);

  upd_iso_msgf(srv->iso, " (%s)\n", srv->name);
}


static void srv_lock_for_access_cb_(upd_file_lock_t* lock) {
  upd_srv_t* srv = lock->udata;
  upd_iso_t* iso = srv->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    srv_logf_(srv, "failed to lock program for access req");
    goto ABORT;
  }

  const bool access = upd_req_with_dup(&(upd_req_t) {
      .file  = srv->prog,
      .type  = UPD_REQ_PROG_ACCESS,
      .cb    = srv_access_cb_,
      .udata = lock,
    });
  if (HEDLEY_UNLIKELY(!access)) {
    srv_logf_(srv, "program refused access req");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
  upd_srv_delete(srv);
}

static void srv_access_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_srv_t*       srv  = lock->udata;
  upd_iso_t*       iso  = srv->iso;

  const bool executable = req->prog.access.exec;
  upd_iso_unstack(iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  if (HEDLEY_UNLIKELY(!executable)) {
    srv_logf_(srv, "server program is not executable");
    goto ABORT;
  }

  const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso   = iso,
      .path  = (uint8_t*) SRV_PATH_,
      .len   = utf8size_lazy(SRV_PATH_),
      .udata = srv,
      .cb    = srv_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!pf)) {
    srv_logf_(srv, "'"SRV_PATH_"' pathfind failure");
    goto ABORT;
  }
  return;

ABORT:
  upd_srv_delete(srv);
}

static void srv_pathfind_cb_(upd_req_pathfind_t* pf) {
  upd_srv_t* srv = pf->udata;
  upd_iso_t* iso = srv->iso;

  upd_file_t* f = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(f == NULL)) {
    srv_logf_(srv, "'"SRV_PATH_"' is not found");
    goto ABORT;
  }

  const bool ok = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = f,
      .ex    = true,
      .udata = srv,
      .cb    = srv_lock_dir_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    srv_logf_(srv, "'"SRV_PATH_"' exlock failure");
    goto ABORT;
  }
  return;

ABORT:
  upd_srv_delete(srv);
}

static void srv_lock_dir_cb_(upd_file_lock_t* lock) {
  upd_srv_t* srv = lock->udata;
  upd_iso_t* iso = srv->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    srv_logf_(srv, "'"SRV_PATH_"' exlock failure");
    goto ABORT;
  }

  srv->dir = upd_file_new(iso, &upd_driver_dir);
  if (HEDLEY_UNLIKELY(srv->dir == NULL)) {
    srv_logf_(srv, "dir creation failure");
    goto ABORT;
  }

  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file = lock->file,
      .type = UPD_REQ_DIR_ADD,
      .dir  = { .entry = {
        .file    = srv->dir,
        .name    = (uint8_t*) srv->name,
        .len     = utf8size_lazy(srv->name),
        .weakref = true,
      }, },
      .udata = lock,
      .cb    = srv_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!add)) {
    srv_logf_(srv, "'"SRV_PATH_"' refused add req");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
  upd_srv_delete(srv);
}

static void srv_add_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_srv_t*       srv  = lock->udata;
  upd_iso_t*       iso  = srv->iso;

  const bool ok = req->dir.entry.file;
  upd_iso_unstack(iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  if (HEDLEY_UNLIKELY(!ok)) {
    srv_logf_(srv, "'"SRV_PATH_"' add req failure");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(0 > uv_listen(&srv->uv.stream, TCP_BACKLOG_, srv_conn_cb_))) {
    srv_logf_(srv, "listen failure");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->srv, srv, SIZE_MAX))) {
    srv_logf_(srv, "registration failure");
    goto ABORT;
  }
  return;

ABORT:
  upd_srv_delete(srv);
}

static void srv_conn_cb_(uv_stream_t* stream, int status) {
  upd_srv_t* srv = (void*) stream;

  if (HEDLEY_UNLIKELY(status < 0)) {
    srv_logf_(srv, "new conn error, %s", uv_err_name(status));
    goto ABORT;
  }

  upd_cli_t* cli = NULL;
  switch (srv->uv.handle.type) {
  case UV_TCP:
    cli = upd_cli_new_tcp(srv);
    break;
  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }

  if (HEDLEY_UNLIKELY(cli == NULL)) {
    srv_logf_(srv, "new client creation failure");
    goto ABORT;
  }
  return;

ABORT:
  upd_srv_delete(srv);
}

static void srv_close_cb_(uv_handle_t* handle) {
  upd_srv_t* srv = (void*) handle;
  upd_free(&srv);
}
