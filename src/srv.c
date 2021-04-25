#include "common.h"


static
void
srv_conn_cb_(
  uv_stream_t* stream,
  int          status);

static
void
srv_shutdown_cb_(
  uv_shutdown_t* shutdown,
  int            status);

static
void
srv_close_cb_(
  uv_handle_t* handle);


upd_srv_t* upd_srv_new_tcp(
    upd_iso_t*     iso,
    upd_file_t*    prog,
    const uint8_t* addr,
    uint16_t       port,
    size_t         backlog) {
  struct sockaddr_in a = {0};
  if (HEDLEY_UNLIKELY(0 > uv_ip4_addr((char*) addr, port, &a))) {
    upd_iso_msgf(iso, "invalid address: %s:%"PRIu16"\n", addr, port);
    return NULL;
  }

  upd_srv_t* srv = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&srv, sizeof(*srv)))) {
    return NULL;
  }
  *srv = (upd_srv_t) {
    .iso  = iso,
    .prog = prog,
  };

  if (HEDLEY_UNLIKELY(0 > uv_tcp_init(&iso->loop, &srv->uv.tcp))) {
    upd_free(&srv);
    return NULL;
  }

  upd_file_ref(prog);

  const bool listen =
    0 <= uv_tcp_bind(&srv->uv.tcp, (struct sockaddr*) &a, 0) &&
    0 <= uv_listen(&srv->uv.stream, backlog, srv_conn_cb_);
  if (HEDLEY_UNLIKELY(!listen)) {
    upd_srv_delete(srv);
    return NULL;
  }

  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->srv, srv, SIZE_MAX))) {
    upd_srv_delete(srv);
    return NULL;
  }

  return srv;
}

void upd_srv_delete(upd_srv_t* srv) {
  upd_array_find_and_remove(&srv->iso->srv, srv);

  uv_shutdown_t* req = upd_iso_stack(srv->iso, sizeof(*req));
  if (HEDLEY_UNLIKELY(req == NULL)) {
    uv_close(&srv->uv.handle, srv_close_cb_);
    return;
  }
  *req = (uv_shutdown_t) { .data = srv, };
  if (HEDLEY_UNLIKELY(0 > uv_shutdown(req, &srv->uv.stream, srv_shutdown_cb_))) {
    upd_iso_unstack(srv->iso, req);
    uv_close(&srv->uv.handle, srv_close_cb_);
    return;
  }
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
  upd_iso_msgf(srv->iso, "new conn established\n");
}

static void srv_shutdown_cb_(uv_shutdown_t* req, int status) {
  (void) status;

  upd_srv_t* srv = req->data;
  upd_iso_unstack(srv->iso, req);
  upd_file_unref(srv->prog);
  uv_close(&srv->uv.handle, srv_close_cb_);
}

static void srv_close_cb_(uv_handle_t* handle) {
  upd_free(&handle);
}
