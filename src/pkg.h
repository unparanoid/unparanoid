#pragma once

#include "common.h"


typedef struct upd_pkg_install_t upd_pkg_install_t;


struct upd_pkg_t {
  uint8_t* name;
  uint8_t* url;
  uint8_t* nrpath;
  uint8_t* npath;
  uint8_t* npath_archive;

  upd_pkg_install_t* install;
};


typedef enum upd_pkg_install_state_t {
  UPD_PKG_INSTALL_MKDIR,
  UPD_PKG_INSTALL_DOWNLOAD,
  UPD_PKG_INSTALL_UNPACK,
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

  uint8_t hash[SHA1_BLOCK_SIZE];
  bool    check_hash;

  void* udata;
  void
  (*cb)(
    upd_pkg_install_t* inst);


  /* output */
  upd_pkg_t* pkg;


  /* internal params */
  upd_pkg_install_state_t state;
  const char*             msg;
};


HEDLEY_NON_NULL(1)
bool
upd_pkg_install(
  upd_pkg_install_t* inst);
