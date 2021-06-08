#pragma once

#include "common.h"


typedef struct upd_pkg_install_t upd_pkg_install_t;


typedef enum upd_pkg_state_t {
  UPD_PKG_INSTALLING,
  UPD_PKG_INSTALLED,
  UPD_PKG_BROKEN,
} upd_pkg_state_t;

struct upd_pkg_t {
  upd_pkg_state_t    state;
  upd_pkg_install_t* install;

  uint8_t* name;
  uint8_t* url;
  uint8_t* nrpath;
  uint8_t* npath;

  uint8_t hash[SHA1_BLOCK_SIZE];

  /* this package is fully trusted and hash isn't calculated */
  unsigned trusted : 1;
};


typedef enum upd_pkg_install_state_t {
  UPD_PKG_INSTALL_MKDIR,
  UPD_PKG_INSTALL_DOWNLOAD,
  UPD_PKG_INSTALL_CONFIGURE,
  UPD_PKG_INSTALL_DONE,
  UPD_PKG_INSTALL_ABORTED,
} upd_pkg_install_state_t;

struct upd_pkg_install_t {
  upd_iso_t* iso;

  const uint8_t* src;
  size_t         srclen;

  const uint8_t* name;
  size_t         namelen;

  const uint8_t* hash;
  size_t         hashlen;

  unsigned verify_ssl : 1;
  unsigned preserve   : 1;  /* don't cleanup broken pkgs */

  void* udata;
  void
  (*cb)(
    upd_pkg_install_t* inst);


  /* output */
  upd_pkg_t* pkg;


  /* internal params */
  upd_pkg_install_state_t state;
  const char*             msg;

  bool abort;
};


HEDLEY_NON_NULL(1)
bool
upd_pkg_install(
  upd_pkg_install_t* inst);

HEDLEY_NON_NULL(1)
void
upd_pkg_abort_install(
  upd_pkg_install_t* inst);
