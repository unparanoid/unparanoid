#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include <hedley.h>
#include <zlib-ng.h>

#include <libupd/memory.h>

#include "png.h"


typedef enum png_reader_state_t {
  PNG_READER_STATE_SIGN         = 0x00,
  PNG_READER_STATE_IHDR         = 0x01,
  PNG_READER_STATE_CHUNK_HEADER = 0x02,
  PNG_READER_STATE_CHUNK_DATA   = 0x03,
  PNG_READER_STATE_CHUNK_FOOTER = 0x04,

  PNG_READER_STATE_DONE  = 0x10,
  PNG_READER_STATE_ERROR = 0x11,
} png_reader_state_t;


typedef struct png_reader_t {
  /* pix.ptr must be a pointer to valid heap or NULL,
   * others must be zero-filled */
  png_reader_state_t state;

  png_ihdr_t ihdr;

  struct {
    uint32_t size;
    uint32_t read;
    uint32_t type;
    uint32_t crc;
    size_t   index;
  } ch;

  struct {
    uint8_t* ptr;
    size_t   size;
    size_t   linesz;
    size_t   pixsz;
    uint8_t  depth;
  } pix;

  struct {
    uint8_t* ptr1;
    uint8_t* ptr2;
    size_t   size;
    size_t   scnt;
    uint8_t  spp;
    uint8_t  bpp;
    size_t   index;
  } line;

  zng_stream z;
  bool       z_done;

  const char* msg;
} png_reader_t;


HEDLEY_NON_NULL(1)
static inline
bool
png_reader_init(
  png_reader_t* re);

HEDLEY_NON_NULL(1)
static inline
uint8_t*  /* OWNERSHIP */
png_reader_deinit(
  png_reader_t* re);

HEDLEY_NON_NULL(1)
static inline
size_t
png_reader_consume(
  png_reader_t*  re,
  const uint8_t* buf,
  size_t         len);


static inline
size_t
png_reader_consume_chunk_(
  png_reader_t*  re,
  const uint8_t* buf,
  size_t         len);

static inline
void
png_reader_scan_line_(
  png_reader_t*  re);


static inline bool png_reader_init(png_reader_t* re) {
  if (HEDLEY_UNLIKELY(Z_OK != zng_inflateInit(&re->z))) {
    return false;
  }
  return true;
}

static inline uint8_t* png_reader_deinit(png_reader_t* re) {
  upd_free(&re->line.ptr1);
  upd_free(&re->line.ptr2);
  zng_inflateEnd(&re->z);
  return re->pix.ptr;
}

static inline size_t png_reader_consume(
    png_reader_t* re, const uint8_t* buf, size_t len) {
  const size_t   in  = len;
  const uint8_t* ptr = NULL;

# define take_(n) do {  \
    if (len < (n)) {  \
      goto EXIT;  \
    }  \
    ptr  = buf;  \
    len -= (n);  \
    buf += (n);  \
  } while (0)

# define throw_(v) do {  \
    re->msg   = (v);  \
    re->state = PNG_READER_STATE_ERROR;  \
    goto EXIT;  \
  } while (0)

BEGIN:
  if (HEDLEY_UNLIKELY(re->state & PNG_READER_STATE_DONE)) {
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(len == 0)) {
    goto EXIT;
  }

  switch (re->state) {
  case PNG_READER_STATE_SIGN:
    take_(8);
    if (*(uint64_t*) ptr != PNG_SIGNATURE) {
      throw_("invalid signature");
    }
    re->state = PNG_READER_STATE_CHUNK_HEADER;
    goto BEGIN;

  case PNG_READER_STATE_CHUNK_HEADER:
    take_(8);

    re->ch.size = png_u32_endian_inline(*(uint32_t*) ptr);
    re->ch.read = 0;

    ptr += 4;
    re->ch.type = *(uint32_t*) ptr;
    re->ch.crc  = png_crc(ptr, 4);

    if (HEDLEY_UNLIKELY(re->ch.index == 0)) {
      if (HEDLEY_UNLIKELY(re->ch.type != PNG_IHDR)) {
        throw_("first chunk must be IHDR");
      }
      if (HEDLEY_UNLIKELY(re->ch.size < sizeof(png_ihdr_t))) {
        throw_("IHDR chunk is too short");
      }
    } else {
      if (HEDLEY_UNLIKELY(re->ch.type == PNG_IHDR)) {
        throw_("IHDR chunk found twice");
      }
      if (HEDLEY_UNLIKELY(re->ch.type == PNG_IEND)) {
        re->state = PNG_READER_STATE_DONE;
        goto EXIT;
      }
    }
    re->state = PNG_READER_STATE_CHUNK_DATA;
    goto BEGIN;

  case PNG_READER_STATE_CHUNK_DATA:
    if (HEDLEY_UNLIKELY(re->ch.size <= re->ch.read)) {
      re->state = PNG_READER_STATE_CHUNK_FOOTER;
      goto BEGIN;
    }
    const size_t rem = re->ch.size - re->ch.read;

    size_t read = len;
    if (read > rem) read = rem;

    re->ch.crc = png_crc_next(re->ch.crc, buf, read);

    read = png_reader_consume_chunk_(re, buf, read);
    if (HEDLEY_UNLIKELY(read == 0)) {
      goto EXIT;
    }

    re->ch.read += read;
    buf += read;
    len -= read;
    goto BEGIN;

  case PNG_READER_STATE_CHUNK_FOOTER:
    take_(4);
    if (HEDLEY_UNLIKELY(~re->ch.crc != png_u32_endian_inline(*(uint32_t*) ptr))) {
      throw_("CRC check failure");
    }
    ++re->ch.index;
    re->state = PNG_READER_STATE_CHUNK_HEADER;
    goto BEGIN;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }

# undef take_
# undef throw_

EXIT:
  return in-len;
}


static inline size_t png_reader_consume_chunk_(
    png_reader_t* re, const uint8_t* buf, size_t len) {

# define throw_(v) do {  \
    re->msg   = (v);  \
    re->state = PNG_READER_STATE_ERROR;  \
    return len;  \
  } while (0)

  switch (re->ch.type) {
  case PNG_IHDR:
    if (HEDLEY_UNLIKELY(len < sizeof(png_ihdr_t))) {
      return 0;
    }
    re->ihdr = *(png_ihdr_t*) buf;
    png_u32_endian(&re->ihdr.width);
    png_u32_endian(&re->ihdr.height);

    if (HEDLEY_UNLIKELY(!png_check_colortype_and_depth(&re->ihdr))) {
      throw_("invalid pair of colortype and depth");
    }
    if (HEDLEY_UNLIKELY(re->ihdr.colortype == PNG_INDEXED)) {
      throw_("unsupported colortype: indexed");
    }
    if (HEDLEY_UNLIKELY(re->ihdr.compression != PNG_ZLIB)) {
      throw_("unknown compression type");
    }
    if (HEDLEY_UNLIKELY(re->ihdr.filter != PNG_NO_FILTER)) {
      throw_("unknown filter type");
    }
    if (HEDLEY_UNLIKELY(re->ihdr.interlace != PNG_NO_INTERLACE)) {
      throw_("interlace is not supported");
    }

    re->line.spp   = png_samples_per_pix(&re->ihdr);
    re->line.bpp   = (re->line.spp * re->ihdr.depth + 7) / 8;
    re->line.scnt  = re->line.spp * re->ihdr.width;
    re->line.size  = png_bytes_line(&re->ihdr);
    if (HEDLEY_UNLIKELY(!upd_malloc(&re->line.ptr1, re->line.size))) {
      throw_("line allocation failure");
    }
    if (HEDLEY_UNLIKELY(!upd_malloc(&re->line.ptr2, re->line.size))) {
      throw_("zero-line allocation failure");
    }
    memset(re->line.ptr2, 0, re->line.size);
    re->z.avail_out = re->line.size;
    re->z.next_out  = re->line.ptr1;

    re->pix.depth  = re->ihdr.depth <= 8? 1: 2;
    re->pix.pixsz  = re->pix.depth * re->line.spp;
    re->pix.linesz = re->pix.pixsz * re->ihdr.width;
    re->pix.size   = re->pix.linesz * re->ihdr.height;
    if (HEDLEY_UNLIKELY(!upd_malloc(&re->pix.ptr, re->pix.size))) {
      throw_("pixbuf allocation failure");
    }
    return len;

  case PNG_IDAT:
    if (HEDLEY_UNLIKELY(re->z_done)) {
      return len;
    }

    re->z.avail_in = len;
    re->z.next_in  = buf;

    while (!re->z_done) {
      const int ret = zng_inflate(&re->z, Z_NO_FLUSH);
      switch (ret) {
      case Z_OK:
        if (HEDLEY_UNLIKELY(re->z.avail_in == 0)) {
          return len;
        }
        assert(re->z.avail_out == 0);
        break;

      case Z_STREAM_END:
        re->z_done = true;
        if (HEDLEY_UNLIKELY(re->z.avail_out)) {
          throw_("data shortage");
        }
        if (HEDLEY_UNLIKELY(re->line.index+1 < re->ihdr.height)) {
          throw_("line shortage");
        }
        break;

      default:
        throw_("zlib error");
      }

      png_reader_scan_line_(re);
      if (HEDLEY_UNLIKELY(++re->line.index >= re->ihdr.height)) {
        re->z_done = true;
        break;
      }
      uint8_t* temp = re->line.ptr2;
      re->line.ptr2 = re->line.ptr1;
      re->line.ptr1 = temp;

      re->z.avail_out = re->line.size;
      re->z.next_out  = re->line.ptr1;
    }
    return len;

  default:
    return len;
  }

# undef throw_
}

static inline void png_reader_scan_line_(png_reader_t* re) {
# define FT_ HEDLEY_FALL_THROUGH;

  const uint8_t filter = re->line.ptr1[0];

  uint8_t*     src   = re->line.ptr1+1;
  const size_t srcsz = re->line.size-1;

  const uint8_t* up        = re->line.ptr2+1;
  uint8_t        left[8]   = {0};
  uint8_t        upleft[8] = {0};

  size_t i = (srcsz+7)/8, j = 0;
  switch (filter) {
  case PNG_RAW:
    break;
  case PNG_SUB:
#   define proc_(n)  \
      switch (srcsz%8) {  \
      case 0: do { left[j%n] = *src += left[j%n]; ++j; ++src; FT_;  \
      case 7:      left[j%n] = *src += left[j%n]; ++j; ++src; FT_;  \
      case 6:      left[j%n] = *src += left[j%n]; ++j; ++src; FT_;  \
      case 5:      left[j%n] = *src += left[j%n]; ++j; ++src; FT_;  \
      case 4:      left[j%n] = *src += left[j%n]; ++j; ++src; FT_;  \
      case 3:      left[j%n] = *src += left[j%n]; ++j; ++src; FT_;  \
      case 2:      left[j%n] = *src += left[j%n]; ++j; ++src; FT_;  \
      case 1:      left[j%n] = *src += left[j%n]; ++j; ++src; FT_;  \
              } while (--i);  \
      }
    switch (re->line.bpp) {
    case 1: proc_(1); break;
    case 2: proc_(2); break;
    case 3: proc_(3); break;
    case 4: proc_(4); break;
    case 5: proc_(5); break;
    case 6: proc_(6); break;
    case 7: proc_(7); break;
    case 8: proc_(8); break;
    }
#   undef proc_
    break;

  case PNG_UP:
#   define proc_(n)  \
      switch (srcsz%8) {  \
      case 0: do { *src += *up; ++src; ++up; FT_;  \
      case 7:      *src += *up; ++src; ++up; FT_;  \
      case 6:      *src += *up; ++src; ++up; FT_;  \
      case 5:      *src += *up; ++src; ++up; FT_;  \
      case 4:      *src += *up; ++src; ++up; FT_;  \
      case 3:      *src += *up; ++src; ++up; FT_;  \
      case 2:      *src += *up; ++src; ++up; FT_;  \
      case 1:      *src += *up; ++src; ++up; FT_;  \
              } while (--i);  \
      }
    switch (re->line.bpp) {
    case 1: proc_(1); break;
    case 2: proc_(2); break;
    case 3: proc_(3); break;
    case 4: proc_(4); break;
    case 5: proc_(5); break;
    case 6: proc_(6); break;
    case 7: proc_(7); break;
    case 8: proc_(8); break;
    }
#   undef proc_
    break;

  case PNG_AVG:
#   define proc_(n)  \
      switch (srcsz%8) {  \
      case 0: do { left[j%n] = *src += (left[j%n]+*up)/2; ++j; ++src; ++up; FT_;  \
      case 7:      left[j%n] = *src += (left[j%n]+*up)/2; ++j; ++src; ++up; FT_;  \
      case 6:      left[j%n] = *src += (left[j%n]+*up)/2; ++j; ++src; ++up; FT_;  \
      case 5:      left[j%n] = *src += (left[j%n]+*up)/2; ++j; ++src; ++up; FT_;  \
      case 4:      left[j%n] = *src += (left[j%n]+*up)/2; ++j; ++src; ++up; FT_;  \
      case 3:      left[j%n] = *src += (left[j%n]+*up)/2; ++j; ++src; ++up; FT_;  \
      case 2:      left[j%n] = *src += (left[j%n]+*up)/2; ++j; ++src; ++up; FT_;  \
      case 1:      left[j%n] = *src += (left[j%n]+*up)/2; ++j; ++src; ++up; FT_;  \
              } while (--i);  \
      }
    switch (re->line.bpp) {
    case 1: proc_(1); break;
    case 2: proc_(2); break;
    case 3: proc_(3); break;
    case 4: proc_(4); break;
    case 5: proc_(5); break;
    case 6: proc_(6); break;
    case 7: proc_(7); break;
    case 8: proc_(8); break;
    }
#   undef proc_
    break;

  case PNG_PAETH:
#   define proc_(n)  \
      switch (srcsz%8) {  \
      case 0: do { left[j%n] = *src += png_paeth(left[j%n], *up, upleft[j%n]); upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
      case 7:      left[j%n] = *src += png_paeth(left[j%n], *up, upleft[j%n]); upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
      case 6:      left[j%n] = *src += png_paeth(left[j%n], *up, upleft[j%n]); upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
      case 5:      left[j%n] = *src += png_paeth(left[j%n], *up, upleft[j%n]); upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
      case 4:      left[j%n] = *src += png_paeth(left[j%n], *up, upleft[j%n]); upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
      case 3:      left[j%n] = *src += png_paeth(left[j%n], *up, upleft[j%n]); upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
      case 2:      left[j%n] = *src += png_paeth(left[j%n], *up, upleft[j%n]); upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
      case 1:      left[j%n] = *src += png_paeth(left[j%n], *up, upleft[j%n]); upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
              } while (--i);  \
      }
    switch (re->line.bpp) {
    case 1: proc_(1); break;
    case 2: proc_(2); break;
    case 3: proc_(3); break;
    case 4: proc_(4); break;
    case 5: proc_(5); break;
    case 6: proc_(6); break;
    case 7: proc_(7); break;
    case 8: proc_(8); break;
    }
#   undef proc_
    break;

  default:
    re->msg   = "invalid filter type";
    re->state = PNG_READER_STATE_ERROR;
    return;
  }

  const uint8_t*  src8  = re->line.ptr1+1;
  const uint16_t* src16 = (uint16_t*) (re->line.ptr1+1);
  const size_t    cnt   = re->line.scnt;

  const size_t offset = re->pix.linesz * re->line.index;
  uint8_t*  dst8  = re->pix.ptr + offset;
  uint16_t* dst16 = (uint16_t*) (re->pix.ptr + offset);

  i = (cnt+7)/8;
  switch (re->ihdr.depth) {
  case 1:
    switch (cnt%8) {
    case 0: do { *(dst8++) = (*src8 & 0x80);              FT_;
    case 7:      *(dst8++) = (*src8 & 0x40) << 1;         FT_;
    case 6:      *(dst8++) = (*src8 & 0x20) << 2;         FT_;
    case 5:      *(dst8++) = (*src8 & 0x10) << 3;         FT_;
    case 4:      *(dst8++) = (*src8 & 0x08) << 4;         FT_;
    case 3:      *(dst8++) = (*src8 & 0x04) << 5;         FT_;
    case 2:      *(dst8++) = (*src8 & 0x02) << 6;         FT_;
    case 1:      *(dst8++) = (*src8 & 0x01) << 7; ++src8; FT_;
            } while (--i);
    }
    break;
  case 2:
    switch (cnt%8) {
    case 0: do { *(dst8++) = (*src8 & 0xC0);              FT_;
    case 7:      *(dst8++) = (*src8 & 0x30) << 2;         FT_;
    case 6:      *(dst8++) = (*src8 & 0x0C) << 4;         FT_;
    case 5:      *(dst8++) = (*src8 & 0x03) << 6; ++src8; FT_;
    case 4:      *(dst8++) = (*src8 & 0xC0);              FT_;
    case 3:      *(dst8++) = (*src8 & 0x30) << 2;         FT_;
    case 2:      *(dst8++) = (*src8 & 0x0C) << 4;         FT_;
    case 1:      *(dst8++) = (*src8 & 0x03) << 6; ++src8; FT_;
            } while (--i);
    }
    break;
  case 4:
    switch (cnt%8) {
    case 0: do { *(dst8++) = (*src8 & 0xF0);              FT_;
    case 7:      *(dst8++) = (*src8 & 0x0F) << 4; ++src8; FT_;
    case 6:      *(dst8++) = (*src8 & 0xF0);              FT_;
    case 5:      *(dst8++) = (*src8 & 0x0F) << 4; ++src8; FT_;
    case 4:      *(dst8++) = (*src8 & 0xF0);              FT_;
    case 3:      *(dst8++) = (*src8 & 0x0F) << 4; ++src8; FT_;
    case 2:      *(dst8++) = (*src8 & 0xF0);              FT_;
    case 1:      *(dst8++) = (*src8 & 0x0F) << 4; ++src8; FT_;
            } while (--i);
    }
    break;
  case 8:
    switch (cnt%8) {
    case 0: do { *(dst8++) = *(src8++); FT_;
    case 7:      *(dst8++) = *(src8++); FT_;
    case 6:      *(dst8++) = *(src8++); FT_;
    case 5:      *(dst8++) = *(src8++); FT_;
    case 4:      *(dst8++) = *(src8++); FT_;
    case 3:      *(dst8++) = *(src8++); FT_;
    case 2:      *(dst8++) = *(src8++); FT_;
    case 1:      *(dst8++) = *(src8++); FT_;
            } while (--i);
    }
    break;
  case 16:
    switch (cnt%8) {
    case 0: do { *(dst16++) = *(src16++); FT_;
    case 7:      *(dst16++) = *(src16++); FT_;
    case 6:      *(dst16++) = *(src16++); FT_;
    case 5:      *(dst16++) = *(src16++); FT_;
    case 4:      *(dst16++) = *(src16++); FT_;
    case 3:      *(dst16++) = *(src16++); FT_;
    case 2:      *(dst16++) = *(src16++); FT_;
    case 1:      *(dst16++) = *(src16++); FT_;
            } while (--i);
    }
    break;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }

# undef FT_
}
