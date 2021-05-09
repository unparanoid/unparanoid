#define UPD_TEST
#undef  NDEBUG

#include "common.h"


typedef struct test_t_ {
  const char* name;

  void
  (*exec)(
    void);
} test_t_;

static const test_t_ tests_[] = {
  { "array",  upd_test_array, },
  { "buf",    upd_test_buf, },
  { "map",    upd_test_map, },
  { "memory", upd_test_memory, },
  { "driver", upd_test_driver, },
  { "file",   upd_test_file, },
  { "path",   upd_test_path, },
  { "req",    upd_test_req, },
  { "srv",    upd_test_srv, },
};


upd_test_t upd_test = {0};


static
uintmax_t
get_wait_time_(
  void);


static
void
timer_cb_(
  uv_timer_t* timer);


int main(int argc, char** argv) {
  argv = uv_setup_args(argc, argv);

  upd_test.iso = upd_iso_new(1024*1024);
  assert(upd_test.iso);

  const size_t n = sizeof(tests_)/sizeof(tests_[0]);
  for (size_t i = 0; i < n; ++i) {
    printf("running tests for '%s'...\n", tests_[i].name);
    tests_[i].exec();
  }
  printf("done\n");

  uv_timer_t timer = {0};
  assert(0 <= uv_timer_init(&upd_test.iso->loop, &timer));
  assert(0 <= uv_timer_start(&timer, timer_cb_, get_wait_time_(), 0));

  printf("starting isolated machine...\n");
  assert(upd_iso_run(upd_test.iso) != UPD_ISO_PANIC);
  printf("isolated machine exited normally\n");
  return EXIT_SUCCESS;
}


static uintmax_t get_wait_time_(void) {
  static const uintmax_t default_ = 1000;

  const char* env = getenv("UPDTEST_WAIT");
  if (env == NULL) {
    return default_;
  }

  char* end;
  const uintmax_t t = strtoumax(env, &end, 10);
  if (HEDLEY_UNLIKELY(t == 0 || t == UINTMAX_MAX)) {
    return default_;
  }
  return t;
}


static void timer_cb_(uv_timer_t* timer) {
  (void) timer;
  upd_iso_exit(upd_test.iso, UPD_ISO_SHUTDOWN);
}
