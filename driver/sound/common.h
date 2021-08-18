#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include <hedley.h>
#include <miniaudio.h>
#include <re.h>
#include <utf8.h>

#define UPD_EXTERNAL_DRIVER
#include <libupd.h>
#undef UPD_EXTERNAL_DRIVER

#include <libupd/memory.h>
#include <libupd/yaml.h>


extern const upd_driver_t snd_dev;


typedef struct snd_dev_t snd_dev_t;


struct snd_dev_t {
  ma_device      ma;
  ma_device_type type;

  ma_mutex mtx;

  float* ring;
  size_t tail;
  size_t head;
  size_t samples;

  bool         verbose;
  uint8_t      name[64];  /* regex pattern */
  bool         found;
  ma_device_id id;

  uint8_t  ch;
  uint32_t srate;
};
