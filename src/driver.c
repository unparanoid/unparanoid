#include "common.h"


static
void
setup_install_(
  upd_file_t*         dir,
  const upd_driver_t* drv);


static
void
setup_install_(
  upd_file_t*         dir,
  const upd_driver_t* drv);


static
void
setup_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
setup_lock_for_add_cb_(
  upd_file_lock_t* lock);

static
void
setup_install_add_cb_(
  upd_req_t* req);


void upd_driver_setup(upd_iso_t* iso) {
  upd_driver_register(iso, &upd_driver_bin_r);
  upd_driver_register(iso, &upd_driver_bin_rw);
  upd_driver_register(iso, &upd_driver_bin_w);
  upd_driver_register(iso, &upd_driver_lua);
  upd_driver_register(iso, &upd_driver_tensor);

  const bool ok = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso  = iso,
      .path = (uint8_t*) "/sys",
      .len  = 4,
      .cb   = setup_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, "pathfind req allocation failure, while driver setup\n");
    return;
  }
}


static void setup_install_(upd_file_t* dir, const upd_driver_t* drv) {
  upd_iso_t* iso = dir->iso;

  upd_file_t* f = upd_file_new(iso, drv);
  if (HEDLEY_UNLIKELY(f == NULL)) {
    upd_iso_msgf(iso,
      "failed to create new file to install driver, '%s'\n", drv->name);
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
      .cb    = setup_install_add_cb_,
    });
  upd_file_unref(f);

  if (HEDLEY_UNLIKELY(!add)) {
    upd_iso_msgf(iso,
      "add request refused, while installing driver, '%s'\n", drv->name);
    return;
  }
}


static void setup_pathfind_cb_(upd_req_pathfind_t* pf) {
  upd_iso_t* iso = pf->iso;

  upd_file_t* sys = pf->len? NULL: pf->base;
  upd_iso_unstack(iso, pf);

  if (HEDLEY_UNLIKELY(sys == NULL)) {
    upd_iso_msgf(iso, "'/sys' is not found, while driver setup\n");
    return;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file = sys,
      .ex   = true,
      .cb   = setup_lock_for_add_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    upd_iso_msgf(iso, "'/sys' dir refused exlock, while driver setup\n");
    return;
  }
}

static void setup_lock_for_add_cb_(upd_file_lock_t* lock) {
  upd_file_t* sys = lock->file;
  upd_iso_t*  iso = sys->iso;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    upd_iso_msgf(iso, "'/sys' dir exlock failure, while driver setup\n");
    goto EXIT;
  }

  setup_install_(sys, &upd_driver_lua_dev);

  setup_install_(sys, &upd_driver_prog_http);
  setup_install_(sys, &upd_driver_prog_parallelism);

EXIT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
}

static void setup_install_add_cb_(upd_req_t* req) {
  upd_iso_t*          iso = req->file->iso;
  const upd_driver_t* drv = req->udata;

  const bool ok = req->dir.entry.file;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, "driver '%s' installation failure\n", drv->name);
    return;
  }
}
