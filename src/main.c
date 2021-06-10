#include "common.h"


static
void
config_load_cb_(
  upd_config_load_t* load);


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
    const bool config = upd_config_load_with_dup(&(upd_config_load_t) {
        .iso   = iso,
        .path  = iso->path.working,
        .feats = UPD_CONFIG_FULL,
        .cb    = config_load_cb_,
      });
    if (HEDLEY_UNLIKELY(!config)) {
      fprintf(stderr, "configuration failure\n");
      return EXIT_FAILURE;
    }

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


static void config_load_cb_(upd_config_load_t* load) {
  upd_iso_t* iso = load->iso;

  const bool ok = load->ok;
  upd_iso_unstack(iso, load);

  if (HEDLEY_LIKELY(ok)) {
    upd_iso_msgf(iso, "#### ---- all done :3 ---- ####\n");
  } else {
    upd_iso_msgf(iso, "XXXX ---- configuration failure ;3 ---- XXXX\n");
    upd_iso_exit(iso, UPD_ISO_PANIC);
  }
}
