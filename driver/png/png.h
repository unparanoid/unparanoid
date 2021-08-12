#pragma once

#include <stdint.h>


#define PNG_PRE_READ_SIZE (8+4+4+0x0D+4)

#define PNG_SIGNATURE 0x0A1A0A0D474E5089

#define PNG_IHDR 0x52444849
#define PNG_IDAT 0x54414449
#define PNG_IEND 0x444e4549

#define PNG_RAW   0
#define PNG_SUB   1
#define PNG_UP    2
#define PNG_AVG   3
#define PNG_PAETH 4


#pragma pack(push, 1)

typedef struct png_ihdr_t {
  uint32_t width;
  uint32_t height;
  uint8_t  depth;

  uint8_t colortype;
# define PNG_GRAYSCALE       0
# define PNG_TRUECOLOR       2
# define PNG_INDEXED         3
# define PNG_GRAYSCALE_ALPHA 4
# define PNG_TRUECOLOR_ALPHA 6

  uint8_t compression;
# define PNG_ZLIB 0

  uint8_t filter;
# define PNG_NO_FILTER 0

  uint8_t interlace;
# define PNG_NO_INTERLACE 0
# define PNG_ADAM7        1
} png_ihdr_t;

_Static_assert(sizeof(png_ihdr_t) == 0x0D, "padding detected in png_ihdr_t");

#pragma pack(pop)


static inline bool png_check_colortype_and_depth(const png_ihdr_t* h) {
  const uint8_t d = h->depth;

  switch (h->colortype) {
  case PNG_GRAYSCALE:
    return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
  case PNG_TRUECOLOR:
    return d == 8 || d == 16;
  case PNG_INDEXED:
    return d == 1 || d == 2 || d == 4 || d == 8;
  case PNG_GRAYSCALE_ALPHA:
    return d == 8 || d == 16;
  case PNG_TRUECOLOR_ALPHA:
    return d == 8 || d == 16;
  }
  return false;
}

static inline size_t png_samples_per_pix(const png_ihdr_t* h) {
  switch (h->colortype) {
  case PNG_GRAYSCALE:
    return 1;
  case PNG_TRUECOLOR:
    return 3;
  case PNG_INDEXED:
    return 1;
  case PNG_GRAYSCALE_ALPHA:
    return 2;
  case PNG_TRUECOLOR_ALPHA:
    return 4;
  }
  return 0;
}

static inline size_t png_bits_per_pix(const png_ihdr_t* h) {
  return h->depth * png_samples_per_pix(h);
}

static inline size_t png_bytes_line(const png_ihdr_t* h) {
  return 1 + (png_bits_per_pix(h)*h->width+7)/8;
}
static inline size_t png_bytes_raw(const png_ihdr_t* h) {
  return (png_bits_per_pix(h)*h->width*h->height+7)/8;
}

static inline uint8_t png_paeth(uint8_t a, uint8_t b, uint8_t c) {
  /* reference: https://darkcrowcorvus.hatenablog.jp/entry/2017/02/12/235044 */

  const int32_t p = (int32_t) a+b-c;

  int32_t pa = p-a;
  int32_t pb = p-b;
  int32_t pc = p-c;

  if (pa < 0) pa = -pa;
  if (pb < 0) pb = -pb;
  if (pc < 0) pc = -pc;

  if (pa <= pb && pa <= pc) {
    return a;
  }
  if (pb <= pc) {
    return b;
  }
  return c;
}

static inline void png_u32_endian(uint32_t* x) {
  *x =
    ((*x >>  0) & 0xFF) << 24 |
    ((*x >>  8) & 0xFF) << 16 |
    ((*x >> 16) & 0xFF) <<  8 |
    ((*x >> 24) & 0xFF) <<  0;
}

static inline uint32_t png_u32_endian_inline(uint32_t x) {
  png_u32_endian(&x);
  return x;
}

static inline uint32_t png_crc_next(uint32_t ret, const uint8_t* b, size_t n) {
# include "crc_table.inc"

  /* https://qiita.com/mikecat_mixc/items/e5d236e3a3803ef7d3c5 */

  for (size_t i = 0; i < n; ++i) {
    ret = png_crc_table[(ret ^ b[i]) & 0xFF] ^ (ret >> 8);
  }
  return ret;
}

static inline uint32_t png_crc(const uint8_t* b, size_t n) {
  uint32_t ret = 0xFFFFFFFF;
  return png_crc_next(ret, b, n);
}
