#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include <hedley.h>
#include <miniaudio.h>
#include <msgpack.h>
#include <re.h>
#include <utf8.h>

#define UPD_EXTERNAL_DRIVER
#include <libupd.h>
#undef UPD_EXTERNAL_DRIVER

#include <libupd/memory.h>
#include <libupd/msgpack.h>
#include <libupd/proto.h>
#include <libupd/yaml.h>


extern const upd_driver_t snd_dev;
extern const upd_driver_t snd_stream;


typedef struct snd_dev_t    snd_dev_t;
typedef struct snd_stream_t snd_stream_t;


struct snd_dev_t {
  ma_device      ma;
  ma_device_type type;

  float* ring;
  size_t tail;
  size_t head;
  size_t samples;

  atomic_uint_least64_t now;  /* in samples */

  bool         verbose;
  uint8_t      name[64];  /* regex pattern */
  bool         found;
  ma_device_id id;

  uint8_t  ch;
  uint32_t srate;
};

struct snd_stream_t {
  upd_file_t* dev;

  uint64_t utime_den;
  uint64_t utime_num;
  uint64_t tbase;

  unsigned init : 1;

  upd_msgpack_t     mpk;
  msgpack_unpacked  upkd;
  upd_proto_parse_t par;
};


HEDLEY_NON_NULL(1, 2)
bool
snd_mix(
  upd_file_t*                  dev,
  const upd_req_tensor_data_t* data,
  uint64_t                     time);
