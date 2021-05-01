#include "common.h"


int main(int argc, char** argv) {
  argv = uv_setup_args(argc, argv);

  for (;;) {
    printf(
      "▄• ▄▌ ▐ ▄  ▄▄▄· ▄▄▄· ▄▄▄   ▄▄▄·  ▐ ▄       ▪  ·▄▄▄▄  \n"
      "█▪██▌•█▌▐█▐█ ▄█▐█ ▀█ ▀▄ █·▐█ ▀█ •█▌▐█▪     ██ ██▪ ██ \n"
      "█▌▐█▌▐█▐▐▌ ██▀·▄█▀▀█ ▐▀▀▄ ▄█▀▀█ ▐█▐▐▌ ▄█▀▄ ▐█·▐█· ▐█▌\n"
      "▐█▄█▌██▐█▌▐█▪·•▐█ ▪▐▌▐█•█▌▐█ ▪▐▌██▐█▌▐█▌.▐▌▐█▌██. ██ \n"
      " ▀▀▀ ▀▀ █▪.▀    ▀  ▀ .▀  ▀ ▀  ▀ ▀▀ █▪ ▀█▄▀▪▀▀▀▀▀▀▀▀• \n");

    upd_iso_t* iso = upd_iso_new(1024*1024*8);
    if (HEDLEY_UNLIKELY(iso == NULL)) {
      fprintf(stderr, "isolated machine creation failure\n");
      return EXIT_FAILURE;
    }

    upd_iso_msgf(iso, "building isolated machine...\n");
    /* TODO */

    const upd_iso_status_t status = upd_iso_run(iso);

    switch (status) {
    case UPD_ISO_PANIC:
      fprintf(stderr, "isolated machine panicked X(\n");
      return EXIT_FAILURE;

    case UPD_ISO_SHUTDOWN:
      printf("isolated machine exited gracefully X)\n");
      return EXIT_SUCCESS;

    case UPD_ISO_REBOOT:
      continue;
    }
  }
}
