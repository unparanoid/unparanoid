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
  { "memory", upd_test_memory, },
  { "driver", upd_test_driver, },
  { "file",   upd_test_file, },
  { "srv",    upd_test_srv, },
};


upd_test_t upd_test = {0};


int main(int argc, char** argv) {
  (void) argc;
  (void) argv;

  upd_test.iso = upd_iso_new(1024*1024);
  assert(upd_test.iso);

  const size_t n = sizeof(tests_)/sizeof(tests_[0]);
  for (size_t i = 0; i < n; ++i) {
    printf("running tests for '%s'...\n", tests_[i].name);
    tests_[i].exec();
  }
  printf("done\n");

  printf("starting isolated machine...\n");
  assert(upd_iso_run(upd_test.iso) != UPD_ISO_PANIC);
  printf("isolated machine exited normally\n");
  return EXIT_SUCCESS;
}
