#include "common.h"


int main(int argc, char** argv) {
  argv = uv_setup_args(argc, argv);
  if (HEDLEY_UNLIKELY(curl_global_init(CURL_GLOBAL_ALL))) {
    fprintf(stderr, "curl init failure\n");
    return EXIT_FAILURE;
  }

  for (;;) {
    printf(
      ".   ..   ..--.  .    .--.     .    .   . .--. --.--.--. \n"
      "|   ||\\  ||   )/ \\   |   )   / \\   |\\  |:    :  |  |   :\n"
      "|   || \\ ||--'/___\\  |--'   /___\\  | \\ ||    |  |  |   |\n"
      ":   ;|  \\||  /     \\ |  \\  /     \\ |  \\|:    ;  |  |   ;\n"
      " `-' '   '' '       `'   `'       `'   ' `--' --'--'--' \n");

    upd_iso_t* iso = upd_iso_new(1024*1024*8);
    if (HEDLEY_UNLIKELY(iso == NULL)) {
      fprintf(stderr, "isolated machine creation failure\n");
      return EXIT_FAILURE;
    }

    upd_iso_msgf(iso, "building isolated machine...\n");
    upd_config_load(iso);

    const upd_iso_status_t status = upd_iso_run(iso);

    switch (status) {
    case UPD_ISO_PANIC:
      fprintf(stderr, "isolated machine panicked X(\n");
      return EXIT_FAILURE;

    case UPD_ISO_SHUTDOWN:
      printf("isolated machine exited gracefully X)\n");
      goto EXIT;

    case UPD_ISO_REBOOT:
      continue;
    }
  }

EXIT:
  curl_global_cleanup();
  return EXIT_SUCCESS;
}
