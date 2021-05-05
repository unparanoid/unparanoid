#pragma once

#include "common.h"


#define UPD_HASH_OUT_SIZE 28


static inline void upd_hash_ptr(
    uint8_t out[UPD_HASH_OUT_SIZE], const void* ptr) {
  uint8_t hashed[SHA1_BLOCK_SIZE];

  SHA1_CTX sha1;
  sha1_init(&sha1);
  sha1_update(&sha1, (uint8_t*) &ptr, sizeof(ptr));
  sha1_final(&sha1, hashed);

  base64_encode(hashed, out, SHA1_BLOCK_SIZE, 0);
}
