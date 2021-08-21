#pragma once

#include "common.h"


#define UPD_ISO_ASYNC_MAX 64


typedef struct upd_iso_thread_t upd_iso_thread_t;
typedef struct upd_iso_work_t   upd_iso_work_t;


struct upd_iso_t {
  uv_loop_t loop;
  uv_tty_t  out;

  uv_signal_t sigint;
  uv_signal_t sighup;

  uv_timer_t shutdown_timer;
  uv_timer_t destroyer;

  upd_iso_status_t status;
  bool             teardown;

  uv_mutex_t mtx;

  upd_array_of(upd_iso_thread_t*) threads;
  upd_array_of(uv_lib_t*)         libs;

  upd_array_of(const upd_driver_t*) drivers;
  upd_array_of(upd_file_t*)         files;
  upd_array_of(upd_pkg_t*)          pkgs;

  size_t files_created;

  struct {
    size_t   used;
    size_t   size;
    size_t   refcnt;
    uint8_t* ptr;
  } stack;

  struct {
    uv_timer_t    timer;
    upd_file_id_t last_seen;

    struct {
      size_t part;
      size_t whole;
      size_t avg;
      size_t thresh;
    } cache;
  } walker;

  struct {
    CURLM*     ctx;
    uv_timer_t timer;
  } curl;

  struct {
    uv_async_t    uv;
    upd_file_id_t id[UPD_ISO_ASYNC_MAX];
    size_t        n;
  } async;

  struct {
    uint8_t runtime[UPD_PATH_MAX];
    uint8_t pkg    [UPD_PATH_MAX];
    uint8_t working[UPD_PATH_MAX];
  } path;
};

struct upd_iso_thread_t {
  uv_thread_t super;

  upd_iso_t* iso;

  void*                 udata;
  upd_iso_thread_main_t main;
};

struct upd_iso_work_t {
  uv_work_t super;

  upd_iso_t* iso;
  void*      udata;

  upd_iso_thread_main_t main;
  upd_iso_work_cb_t     cb;
};


/* Application must exit immediately if this function fails. */
upd_iso_t*
upd_iso_new(
  size_t stacksz);

/* The isolated instance is deleted automatically after running. */
HEDLEY_NON_NULL(1)
upd_iso_status_t
upd_iso_run(
  upd_iso_t* iso);

HEDLEY_NON_NULL(1)
void
upd_iso_exit(
  upd_iso_t*       iso,
  upd_iso_status_t status);


typedef
void
(*upd_iso_curl_cb_t)(
  CURL*    curl,
  CURLcode status,
  void*    udata);

HEDLEY_NON_NULL(1, 2)
bool
upd_iso_curl_perform(
  upd_iso_t*        iso,
  CURL*             curl,
  upd_iso_curl_cb_t cb,
  void*             udata);


static inline void* upd_iso_stack(upd_iso_t* iso, uint64_t len) {
  if (HEDLEY_UNLIKELY(iso->stack.used+len > iso->stack.size || len > 1024*4)) {
    void* ptr = NULL;
    if (HEDLEY_UNLIKELY(!upd_malloc(&ptr, len))) {
      return NULL;
    }
    return ptr;
  }

  void* ret = iso->stack.ptr + iso->stack.used;
  iso->stack.used += len;
  ++iso->stack.refcnt;

# if UPD_USE_VALGRIND
    VALGRIND_MALLOCLIKE_BLOCK(ret, len, 0, 0);
# endif
  return ret;
}

static inline void upd_iso_unstack(upd_iso_t* iso, void* ptr) {
  (void) ptr;

  void* begin = iso->stack.ptr;
  void* end   = iso->stack.ptr + iso->stack.size;

  if (HEDLEY_UNLIKELY(ptr < begin || end <= ptr)) {
    upd_free(&ptr);
    return;
  }
  assert(iso->stack.refcnt);

# if UPD_USE_VALGRIND
    VALGRIND_FREELIKE_BLOCK(ptr, 0);
# endif

  if (--iso->stack.refcnt == 0) {
    iso->stack.used = 0;
  }
}

static inline uint64_t upd_iso_now(upd_iso_t* iso) {
  return uv_now(&iso->loop);
}

static void upd_iso_msg_write_cb_(uv_write_t* req, int status) {
  upd_iso_unstack(req->data, req);
  if (HEDLEY_UNLIKELY(status < 0)) {
    fprintf(stderr, "failed to write msg to stdout\n");
    return;
  }
}
static inline void upd_iso_msg(upd_iso_t* iso, const uint8_t* msg, uint64_t len) {
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

static inline void upd_iso_thread_entrypoint_(void* udata) {
  upd_iso_thread_t* th  = udata;
  upd_iso_t*        iso = th->iso;

  th->main(th->udata);

  uv_mutex_lock(&iso->mtx);
  upd_array_find_and_remove(&iso->threads, th);
  uv_mutex_unlock(&iso->mtx);
  free(th);
}
static inline bool upd_iso_start_thread(
    upd_iso_t* iso, upd_iso_thread_main_t main, void* udata) {
  /*  Implementations of upd_malloc and upd_free have no
   * guarantee that they're thread safe */
  upd_iso_thread_t* th = malloc(sizeof(*th));
  if (HEDLEY_UNLIKELY(th == NULL)) {
    return false;
  }
  *th = (upd_iso_thread_t) {
    .iso   = iso,
    .udata = udata,
    .main  = main,
  };

  uv_mutex_lock(&iso->mtx);
  const bool insert = upd_array_insert(&iso->threads, th, SIZE_MAX);
  uv_mutex_unlock(&iso->mtx);

  if (HEDLEY_UNLIKELY(!insert)) {
    free(th);
    return false;
  }

  const bool start = 0 <= uv_thread_create(
    &th->super, upd_iso_thread_entrypoint_, th);
  if (HEDLEY_UNLIKELY(!start)) {
    uv_mutex_lock(&iso->mtx);
    upd_array_find_and_remove(&iso->threads, th);
    uv_mutex_unlock(&iso->mtx);
    free(th);
    return false;
  }
  return true;
}

static inline void upd_iso_work_entrypoint_(uv_work_t* work) {
  upd_iso_work_t* w = (void*) work;
  w->main(w->udata);
}
static inline void upd_iso_work_after_cb_(uv_work_t* work, int status) {
  (void) status;

  upd_iso_work_t* w = (void*) work;
  w->cb(w->iso, w->udata);
  upd_iso_unstack(w->iso, w);
}
static inline bool upd_iso_start_work(
    upd_iso_t*            iso,
    upd_iso_thread_main_t main,
    upd_iso_work_cb_t     cb,
    void*                 udata) {
  upd_iso_work_t* w = upd_iso_stack(iso, sizeof(*w));
  if (HEDLEY_UNLIKELY(w == NULL)) {
    return false;
  }
  *w = (upd_iso_work_t) {
    .iso   = iso,
    .udata = udata,
    .main  = main,
    .cb    = cb,
  };
  const bool q = 0 <= uv_queue_work(
    &iso->loop, &w->super, upd_iso_work_entrypoint_, upd_iso_work_after_cb_);
  if (HEDLEY_UNLIKELY(!q)) {
    upd_iso_unstack(iso, w);
    return false;
  }
  return true;
}
