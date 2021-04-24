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
  return iso;
}

upd_iso_status_t upd_iso_run(upd_iso_t* iso) {
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
