#include "common.h"


upd_iso_t* upd_iso_new(size_t stacksz) {
  upd_iso_t* iso = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&iso, sizeof(*iso)+stacksz))) {
    return NULL;
  }
  *iso = (upd_iso_t) {
    .stack = {
      .size = stacksz,
      .ptr  = (uint8_t*) (iso+1),
    },
  };

  if (HEDLEY_UNLIKELY(0 > uv_loop_init(&iso->loop))) {
    upd_free(&iso);
    return NULL;
  }

  if (HEDLEY_LIKELY(0 <= uv_tty_init(&iso->loop, &iso->out, 1, 0))) {
    iso->out_ok = true;
  } else {
    fprintf(stderr, "stdout tty object creation failure\n");
  }
  return iso;
}

upd_iso_status_t upd_iso_run(upd_iso_t* iso) {
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  uv_close((uv_handle_t*) &iso->out, NULL);
  if (HEDLEY_UNLIKELY(0 > uv_run(&iso->loop, UV_RUN_DEFAULT))) {
    return UPD_ISO_PANIC;
  }

  if (HEDLEY_UNLIKELY(0 > uv_loop_close(&iso->loop))) {
    return UPD_ISO_PANIC;
  }
  const upd_iso_status_t ret = iso->status;
  upd_free(iso);
  return ret;
}
