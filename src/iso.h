#pragma once

#include "common.h"


struct upd_iso_t {
  uv_loop_t loop;

  bool      out_ok;
  uv_tty_t  out;

  upd_iso_status_t status;

  upd_array_of(const upd_driver_t*) drivers;
  upd_array_of(upd_file_t*)         files;
  upd_array_of(upd_srv_t*)          srv;
  upd_array_of(upd_cli_t*)          cli;

  size_t files_created;

  struct {
    size_t   used;
    size_t   size;
    size_t   refcnt;
    uint8_t* ptr;
  } stack;
};


/* Application must exit immediately if this function fails. */
upd_iso_t*
upd_iso_new(
  size_t stacksz);

/* The isolated instance is deleted after running. */
HEDLEY_NON_NULL(1)
upd_iso_status_t
upd_iso_run(
  upd_iso_t* iso);

HEDLEY_NON_NULL(1)
void
upd_iso_close_all_conn(
  upd_iso_t* iso);


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline void* upd_iso_stack(upd_iso_t* iso, uint64_t len) {
  if (HEDLEY_UNLIKELY(iso->stack.used+len > iso->stack.size)) {
    return NULL;
  }

  void* ret = iso->stack.ptr + iso->stack.used;
  iso->stack.used += len;
  ++iso->stack.refcnt;

  return ret;
}

HEDLEY_NON_NULL(1)
static inline void upd_iso_unstack(upd_iso_t* iso, void* ptr) {
  (void) ptr;
  assert(iso->stack.refcnt);

  if (--iso->stack.refcnt == 0) {
    iso->stack.used = 0;
  }
}

static void upd_iso_msg_write_cb_(uv_write_t* req, int status) {
  upd_iso_unstack(req->data, req);
  if (HEDLEY_UNLIKELY(status < 0)) {
    fprintf(stderr, "failed to write msg to stdout\n");
    return;
  }
}
HEDLEY_NON_NULL(1)
static inline void upd_iso_msg(upd_iso_t* iso, const uint8_t* msg, uint64_t len) {
  if (HEDLEY_UNLIKELY(!iso->out_ok)) {
    return;
  }

  uv_write_t* req = upd_iso_stack(iso, sizeof(*req)+len);
  if (HEDLEY_UNLIKELY(req == NULL)) {
    fprintf(stderr, "msg allocation failure\n");
    return;
  }

  *req = (uv_write_t) { .data = iso, };
  memcpy(req+1, msg, len);

  const uv_buf_t buf = uv_buf_init((char*) (req+1), len);

  const int err =
    uv_write(req, (uv_stream_t*) &iso->out, &buf, 1, upd_iso_msg_write_cb_);
  if (HEDLEY_UNLIKELY(err < 0)) {
    upd_iso_unstack(iso, req);
    fprintf(stderr, "failed to write msg to stdout\n");
    return;
  }
}

HEDLEY_NON_NULL(1)
HEDLEY_PRINTF_FORMAT(2, 3)
static inline void upd_iso_msgf(upd_iso_t* iso, const char* fmt, ...) {
  uint8_t buf[1024];

  va_list args;
  va_start(args, fmt);
  const size_t len = vsnprintf((char*) buf, sizeof(buf), fmt, args);
  va_end(args);

  upd_iso_msg(iso, buf, len);
}

HEDLEY_NON_NULL(1)
static inline void upd_iso_exit(upd_iso_t* iso, upd_iso_status_t status) {
  iso->status = status;
  upd_iso_close_all_conn(iso);
}
