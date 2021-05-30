#include "common.h"


#define EXTERNAL_DRIVER_SYMBOL_ "upd"

#if defined(__unix__)
# define EXTERNAL_DRIVER_EXT_ ".x86_64.so"
#elif defined(_WIN32) || defined(_WIN64)
# define EXTERNAL_DRIVER_EXT_ ".x86_64.dll"
#endif


static const upd_host_t host_ = {
  .iso = {
    .stack        = upd_iso_stack,
    .unstack      = upd_iso_unstack,
    .now          = upd_iso_now,
    .msg          = upd_iso_msg,
    .start_thread = upd_iso_start_thread,
    .start_work   = upd_iso_start_work,
  },
  .driver = {
    .lookup = upd_driver_lookup,
  },
  .file = {
    .new           = upd_file_new,
    .get           = upd_file_get,
    .ref           = upd_file_ref,
    .unref         = upd_file_unref,
    .watch         = upd_file_watch,
    .unwatch       = upd_file_unwatch,
    .trigger       = upd_file_trigger,
    .lock          = upd_file_lock,
    .unlock        = upd_file_unlock,
    .trigger_async = upd_file_trigger_async,
  },
  .req = upd_req,
};


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


static
void
load_work_cb_(
  uv_work_t* w);

static
void
load_work_after_cb_(
  uv_work_t* w,
  int        status);


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

bool upd_driver_load_external(upd_driver_load_external_t* load) {
  upd_iso_t* iso = load->iso;

  load->lib = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&load->lib, sizeof(*load->lib)))) {
    return false;
  }

  uv_work_t* work = upd_iso_stack(iso, sizeof(*work));
  if (HEDLEY_UNLIKELY(work == NULL)) {
    upd_free(&load->lib);
    return false;
  }
  *work = (uv_work_t) { .data = load, };

  const int err =
    uv_queue_work(&iso->loop, work, load_work_cb_, load_work_after_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    upd_free(&load->lib);
    upd_iso_unstack(iso, work);
    return false;
  }
  return true;
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

  const bool ok = req->result == UPD_REQ_OK;
  upd_iso_unstack(iso, req);

  if (HEDLEY_UNLIKELY(!ok)) {
    upd_iso_msgf(iso, "driver '%s' installation failure\n", drv->name);
    return;
  }
}


static void load_work_cb_(uv_work_t* w) {
  upd_driver_load_external_t* load = w->data;

  const size_t n = load->npathlen + sizeof(EXTERNAL_DRIVER_EXT_);
  if (HEDLEY_UNLIKELY(n > UPD_PATH_MAX)) {
    load->err = "too long path";
    return;
  }

  uint8_t path[UPD_PATH_MAX];
  utf8ncpy(path, load->npath, load->npathlen);
  path[load->npathlen] = 0;
  utf8cat(path, EXTERNAL_DRIVER_EXT_);

  const int err = uv_dlopen((char*) path, load->lib);
  if (HEDLEY_UNLIKELY(0 > err)) {
    load->err = uv_dlerror(load->lib);
    return;
  }
}

static void load_work_after_cb_(uv_work_t* w, int status) {
  upd_driver_load_external_t* load = w->data;
  upd_iso_t*                  iso  = load->iso;

  upd_iso_unstack(iso, w);
  if (HEDLEY_UNLIKELY(status < 0)) {
    upd_iso_msgf(iso, "failed to load dynamic library: %s\n", load->npath);
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(load->err)) {
    upd_iso_msgf(iso,
      "failed to load dynamic library: %s (%s)\n", load->npath, load->err);
    goto ABORT;
  }

  uv_lib_t* lib = load->lib;

  upd_external_t* extdrv;
  int err = uv_dlsym(lib, EXTERNAL_DRIVER_SYMBOL_, (void*) &extdrv);
  if (HEDLEY_UNLIKELY(0 > err)) {
    uv_dlclose(lib);
    upd_iso_msgf(iso,
      "symbol '"EXTERNAL_DRIVER_SYMBOL_"' is not found: %s\n", load->npath);
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->libs, load->lib, SIZE_MAX))) {
    uv_dlclose(load->lib);
    upd_iso_msgf(iso, "external library insertion failure: %s\n", load->npath);
    goto ABORT;
  }

  extdrv->host = &host_;
  for (const upd_driver_t** d = extdrv->drivers; *d; ++d) {
    upd_driver_register(iso, *d);
  }
  load->ok = true;
  goto EXIT;

ABORT:
  upd_free(&load->lib);

EXIT:
  load->cb(load);
}
