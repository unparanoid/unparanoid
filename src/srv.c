#include "common.h"


#define TCP_BACKLOG_ 256


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
    upd_iso_msgf(iso, "invalid address: %s:%"PRIu16"\n", addr, port);
    return NULL;
  }

  char name[32] = {0};
  const size_t namelen = snprintf(name, sizeof(name), "tcp-%"PRIu16, port);

  upd_srv_t* srv = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&srv, sizeof(*srv)+namelen+1))) {
    upd_iso_msgf(iso, "server allocation failure\n");
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
    upd_free(&srv);
    upd_iso_msgf(iso, "tcp server allocation failure\n");
    return NULL;
  }

  upd_file_ref(srv->prog);

  if (HEDLEY_UNLIKELY(0 > uv_tcp_bind(&srv->uv.tcp, (struct sockaddr*) &a, 0))) {
    upd_srv_delete(srv);
    upd_iso_msgf(iso, "tcp bind failure (%s:%"PRIu16")\n", addr, port);
    return NULL;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = prog,
      .udata = srv,
      .cb    = srv_lock_for_access_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_srv_delete(srv);
    upd_iso_msgf(iso, "server program lock allocation failure in access request\n");
    return NULL;
  }
  return srv;
}

void upd_srv_delete(upd_srv_t* srv) {
  upd_array_find_and_remove(&srv->iso->srv, srv);
  uv_close(&srv->uv.handle, srv_close_cb_);
}


static void srv_lock_for_access_cb_(upd_file_lock_t* lock) {
  upd_srv_t* srv = lock->udata;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    upd_iso_msgf(srv->iso, "failed in lock of server program for access request\n");
    goto ABORT;
  }

  const bool access = upd_req_with_dup(&(upd_req_t) {
      .file  = srv->prog,
      .type  = UPD_REQ_PROGRAM_ACCESS,
      .cb    = srv_access_cb_,
      .udata = lock,
    });
  if (HEDLEY_UNLIKELY(!access)) {
    upd_iso_msgf(srv->iso, "server program refused access request\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(srv->iso, lock);
  upd_srv_delete(srv);
}

static void srv_access_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_srv_t*       srv  = lock->udata;

  const bool executable = req->program.access.exec;
  upd_iso_unstack(srv->iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(srv->iso, lock);

  if (HEDLEY_UNLIKELY(!executable)) {
    upd_iso_msgf(srv->iso, "server program is not executable\n");
    goto ABORT;
  }

  srv->dir = upd_file_new(srv->iso, &upd_driver_dir);
  if (HEDLEY_UNLIKELY(srv->dir == NULL)) {
    upd_iso_msgf(srv->iso, "server directory allocation failure\n");
    goto ABORT;
  }
  const bool ok = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = upd_file_get(srv->iso, UPD_FILE_ID_ROOT),
      .ex    = true,
      .udata = srv,
      .cb    = srv_lock_dir_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(srv->iso, "server directory lock allocation failure\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_srv_delete(srv);
}

static void srv_lock_dir_cb_(upd_file_lock_t* l) {
  upd_srv_t* srv = l->udata;

  if (HEDLEY_UNLIKELY(!l->ok)) {
    upd_iso_msgf(srv->iso, "server directory lock failure\n");
    goto ABORT;
  }
  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file = l->file,
      .type = UPD_REQ_DIR_ADD,
      .dir  = { .entry = {
        .file    = srv->dir,
        .name    = (uint8_t*) srv->name,
        .len     = utf8size_lazy(srv->name),
        .weakref = true,
      }, },
      .udata = l,
      .cb    = srv_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!add)) {
    upd_iso_msgf(srv->iso, "server directory refused add request\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(l);
  upd_iso_unstack(srv->iso, l);
  upd_srv_delete(srv);
}

static void srv_add_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_iso_t*       iso  = req->file->iso;
  upd_srv_t*       srv  = lock->udata;

  const bool ok = req->dir.entry.file;
  upd_iso_unstack(iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, "server directory add request failure\n");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(0 > uv_listen(&srv->uv.stream, TCP_BACKLOG_, srv_conn_cb_))) {
    upd_iso_msgf(iso, "server listen failure\n");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->srv, srv, SIZE_MAX))) {
    upd_iso_msgf(iso, "failed to register server to isolated machine\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_srv_delete(srv);
}

static void srv_conn_cb_(uv_stream_t* stream, int status) {
  upd_srv_t* srv = (void*) stream;

  if (HEDLEY_UNLIKELY(status < 0)) {
    upd_iso_msgf(srv->iso, "server listen error: %s\n", uv_err_name(status));
    upd_srv_delete(srv);
    return;
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
    upd_iso_msgf(srv->iso, "client creation failure\n");
    upd_srv_delete(srv);
    return;
  }
}

static void srv_close_cb_(uv_handle_t* handle) {
  upd_srv_t* srv = (void*) handle;
  if (HEDLEY_LIKELY(srv->dir)) {
    upd_file_unref(srv->dir);
  }
  upd_file_unref(srv->prog);
  upd_free(&srv);
}
