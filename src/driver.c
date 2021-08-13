#include "common.h"


#define EXTERNAL_DRIVER_SYMBOL_ "upd"

#if defined(__unix__)
# define EXTERNAL_DRIVER_EXT_ ".x86_64.so"
#elif defined(_WIN32) || defined(_WIN64)
# define EXTERNAL_DRIVER_EXT_ ".x86_64.dll"
#endif

static const upd_host_t host_ = UPD_HOST_INSTANCE;


static
void
setup_pathfind_cb_(
  upd_pathfind_t* pf);

static
void
setup_lock_for_add_cb_(
  upd_file_lock_t* lock);


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
  const bool reg =
    upd_driver_register(iso, &upd_driver_bin) &&
    upd_driver_register(iso, &upd_driver_factory) &&
    upd_driver_register(iso, &upd_driver_srv_tcp) &&
    upd_driver_register(iso, &upd_driver_syncdir);
  if (HEDLEY_UNLIKELY(!reg)) {
    upd_iso_msgf(iso, "system driver registration failure\n");
    return;
  }

  const bool ok = upd_pathfind_with_dup(&(upd_pathfind_t) {
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


static void setup_pathfind_cb_(upd_pathfind_t* pf) {
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

EXIT:
  upd_file_unlock(lock);
  upd_iso_unstack(iso, lock);
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

  upd_external_t* ext;
  int err = uv_dlsym(lib, EXTERNAL_DRIVER_SYMBOL_, (void*) &ext);
  if (HEDLEY_UNLIKELY(0 > err)) {
    uv_dlclose(lib);
    upd_iso_msgf(iso,
      "symbol '"EXTERNAL_DRIVER_SYMBOL_"' is not found: %s\n", load->npath);
    goto ABORT;
  }

  const uint16_t maj = ext->ver >> 16;
  const uint16_t min = ext->ver & 0xFFFF;
  if (HEDLEY_UNLIKELY(maj != UPD_VER_MAJOR)) {
    uv_dlclose(lib);
    upd_iso_msgf(iso,
      "major version unmatch upd:%"PRIu16" != %"PRIu16":%s\n",
      UPD_VER_MAJOR, maj, load->npath);
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(min != UPD_VER_MINOR)) {
    upd_iso_msgf(iso,
      "minor version unmatch upd:%"PRIu16" != %"PRIu16":%s\n",
      UPD_VER_MINOR, min, load->npath);
  }

  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->libs, load->lib, SIZE_MAX))) {
    uv_dlclose(load->lib);
    upd_iso_msgf(iso, "external library insertion failure: %s\n", load->npath);
    goto ABORT;
  }

  ext->host = &host_;
  for (const upd_driver_t** d = ext->drivers; *d; ++d) {
    if (HEDLEY_UNLIKELY(!upd_driver_register(iso, *d))) {
      upd_iso_msgf(iso, "registration failure: %s\n", (*d)->name);
    }
  }
  load->ok = true;
  goto EXIT;

ABORT:
  upd_free(&load->lib);

EXIT:
  load->cb(load);
}
