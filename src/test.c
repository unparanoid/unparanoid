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
};


int main(int argc, char** argv) {
  (void) argc;
  (void) argv;

  const size_t n = sizeof(tests_)/sizeof(tests_[0]);
  for (size_t i = 0; i < n; ++i) {
    printf("running tests for '%s'...\n", tests_[i].name);
    tests_[i].exec();
  }
  printf("done\n");
  return EXIT_SUCCESS;
}
