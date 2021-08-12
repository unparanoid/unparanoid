#include "common.h"

upd_external_t upd = {
  .ver = UPD_VER,
  .drivers = (const upd_driver_t*[]) {
    &lj_dev,
    &lj_prog,
    NULL,
  },
};
