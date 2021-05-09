#include "common.h"


static
void
setup_add_(
  upd_file_t*         dir,
  const upd_driver_t* drv);


void upd_driver_setup_iso(upd_iso_t* iso) {
  upd_file_t* dev = upd_file_get(iso, UPD_FILE_ID_DEV);
  if (HEDLEY_LIKELY(dev)) {
    setup_add_(dev, &upd_driver_dev_duktape);
  } else {
    upd_iso_msgf(iso,
      "/dev dir is not found, skipping setting up device drivers\n");
  }
}


static void setup_add_cb_(upd_req_t* req) {
  upd_iso_t*          iso = req->file->iso;
  const upd_driver_t* drv = req->udata;

  const bool ok = req->dir.entry.file;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso,
      "failed to add new file, '%s', while drivers setup\n", drv->name);
    return;
  }
}
static void setup_add_(
    upd_file_t* dir, const upd_driver_t* drv) {
  upd_iso_t* iso = dir->iso;

  upd_file_t* f = upd_file_new(dir->iso, drv);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    upd_iso_msgf(iso,
      "failed to create new file with driver, '%s', while drivers setup\n",
      drv->name);
    return;
  }

  const bool add = upd_req_with_dup(&(upd_req_t) {
      .file = dir,
      .type = UPD_REQ_DIR_ADD,
      .dir  = { .entry = {
        .file = f,
        .name = (uint8_t*) drv->name,
        .len  = utf8size_lazy(drv->name),
      }, },
      .udata = (void*) drv,
      .cb    = setup_add_cb_,
    });
  upd_file_unref(f);
  if (HEDLEY_UNLIKELY(!add)) {
    upd_iso_msgf(iso,
      "dir refused adding new file, '%s', while drivers setup\n",
      drv->name);
    return;
  }
}
