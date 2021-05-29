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
srv_conn_cb_(
  uv_stream_t* stream,
  int          status);

static
void
srv_close_cb_(
  uv_handle_t* handle);


static
void
srv_build_pathfind_prog_cb_(
  upd_req_pathfind_t* pf);

static
void
srv_build_lock_for_access_cb_(
  upd_file_lock_t*);

static
void
srv_build_access_cb_(
  upd_req_t* req);

static
void
srv_build_pathfind_dir_cb_(
  upd_req_pathfind_t* pf);


void upd_srv_delete(upd_srv_t* srv) {
  upd_array_find_and_remove(&srv->iso->srv, srv);

  upd_file_unref(srv->dir);
  upd_file_unref(srv->prog);

  uv_close(&srv->uv.handle, srv_close_cb_);
}


bool upd_srv_build(upd_srv_build_t* b) {
  b->srv = NULL;
  const bool pf = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso   = b->iso,
      .path  = (uint8_t*) b->path,
      .len   = b->pathlen,
      .udata = b,
      .cb    = srv_build_pathfind_prog_cb_,
    });
  if (HEDLEY_UNLIKELY(!pf)) {
    return false;
  }
  return true;
}


static void srv_logf_(upd_srv_t* srv, const char* fmt, ...) {
  upd_iso_msgf(srv->iso, "srv error: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(srv->iso, fmt, args);
  va_end(args);

  upd_iso_msgf(srv->iso, " (%s)\n", srv->name);
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


static void srv_build_pathfind_prog_cb_(upd_req_pathfind_t* pf) {
  upd_srv_build_t* b   = pf->udata;
  upd_iso_t*       iso = pf->iso;

  b->prog = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(b->prog == NULL)) {
    upd_iso_msgf(iso, "program not found: %.*s\n", (int) b->pathlen, b->path);
    goto ABORT;
  }

  upd_file_ref(b->prog);
  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = b->prog,
      .udata = b,
      .cb    = srv_build_lock_for_access_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_file_unref(b->prog);
    upd_iso_msgf(iso, "failed to lock program file\n");
    goto ABORT;
  }
  return;

ABORT:
  b->cb(b);
}

static void srv_build_lock_for_access_cb_(upd_file_lock_t* lock) {
  upd_srv_build_t* b    = lock->udata;
  upd_iso_t*       iso  = b->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    upd_iso_msgf(iso, "failed to lock program for access req\n");
    goto ABORT;
  }
  const bool ok = upd_req_with_dup(&(upd_req_t) {
      .file  = b->prog,
      .type  = UPD_REQ_PROG_ACCESS,
      .udata = lock,
      .cb    = srv_build_access_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, "program access req refused\n");
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  upd_file_unref(b->prog);
  b->cb(b);
}

static void srv_build_access_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  upd_srv_build_t* b    = lock->udata;
  upd_iso_t*       iso  = b->iso;

  const bool exec = req->prog.access.exec;
  upd_iso_unstack(iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);

  if (HEDLEY_UNLIKELY(!exec)) {
    upd_iso_msgf(iso, "program is not executable\n");
    goto ABORT;
  }

  uint8_t name[256];
  const size_t namelen = snprintf(
    (char*) name, sizeof(name),
    "%.*s-%"PRIu16, (int) b->hostlen, b->host, b->port);
  if (HEDLEY_UNLIKELY(namelen+1 >= sizeof(name))) {
    upd_iso_msgf(iso, "too long hostname: %.*s\n", (int) b->hostlen, b->host);
    goto ABORT;
  }

  const size_t len = cwk_path_join(SRV_PATH_, (char*) name, NULL, 0);

  upd_req_pathfind_t* pf = upd_iso_stack(iso, sizeof(*pf)+len+1);
  if (HEDLEY_UNLIKELY(pf == NULL)) {
    upd_iso_msgf(iso, "pathfind req allocation failure\n");
    goto ABORT;
  }
  *pf = (upd_req_pathfind_t) {
    .iso    = b->iso,
    .path   = (uint8_t*) (pf+1),
    .len    = len,
    .create = true,
    .udata  = b,
    .cb     = srv_build_pathfind_dir_cb_,
  };
  cwk_path_join(SRV_PATH_, (char*) name, (char*) pf->path, len+1);
  upd_req_pathfind(pf);
  return;

ABORT:
  upd_file_unref(b->prog);
  b->cb(b);
}

static void srv_build_pathfind_dir_cb_(upd_req_pathfind_t* pf) {
  upd_srv_build_t* b   = pf->udata;
  upd_iso_t*       iso = pf->iso;

  b->dir = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(b->dir == NULL)) {
    upd_iso_msgf(iso, "server directory creation failure\n");
    goto ABORT;
  }

  uint8_t* host = upd_iso_stack(iso, b->hostlen+1);
  if (HEDLEY_UNLIKELY(host == NULL)) {
    upd_iso_msgf(iso, "host address allocation failure\n");
    goto ABORT;
  }
  utf8ncpy(host, b->host, b->hostlen);
  host[b->hostlen] = 0;

  struct sockaddr_in addr = {0};
  if (HEDLEY_UNLIKELY(0 > uv_ip4_addr((char*) b->host, b->port, &addr))) {
    upd_iso_unstack(iso, host);
    upd_iso_msgf(iso, "invalid address\n");
    goto ABORT;
  }
  upd_iso_unstack(iso, host);

  upd_srv_t* srv = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&srv, sizeof(*b->srv)))) {
    upd_iso_msgf(iso, "server allocation failure\n");
    goto ABORT;
  }
  *srv = (upd_srv_t) {
    .iso = iso,
  };
  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&iso->loop, &srv->uv.tcp))) {
    upd_iso_msgf(iso, "tcp handle allocation failure\n");
    upd_free(&b);
    goto ABORT;
  }

  srv->prog = b->prog;
  srv->dir  = b->dir;
  upd_file_ref(b->dir);

  const int bind = uv_tcp_bind(&srv->uv.tcp, (struct sockaddr*) &addr, 0);
  if (HEDLEY_UNLIKELY(0 > bind)) {
    upd_iso_msgf(iso, "tcp bind failure\n");
    upd_srv_delete(srv);
    goto EXIT;
  }
  const int listen = uv_listen(&srv->uv.stream, TCP_BACKLOG_, srv_conn_cb_);
  if (HEDLEY_UNLIKELY(0 > listen)) {
    upd_iso_msgf(iso, "tcp listen failure\n");
    upd_srv_delete(srv);
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->srv, srv, SIZE_MAX))) {
    upd_iso_msgf(iso, "server insertion failure\n");
    upd_srv_delete(srv);
    goto EXIT;
  }
  b->srv = srv;
  goto EXIT;

ABORT:
  upd_file_unref(b->prog);

EXIT:
  b->cb(b);
}
