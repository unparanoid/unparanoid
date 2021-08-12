#pragma once

#define PNG_WRITER_INPUT_MIN 256


typedef enum png_writer_state_t {
  PNG_WRITER_STATE_IHDR,
  PNG_WRITER_STATE_IDAT,
  PNG_WRITER_STATE_IEND,

  PNG_WRITER_STATE_DONE  = 0x10,
  PNG_WRITER_STATE_ERROR = 0x11,
} png_writer_state_t;

typedef struct png_writer_t {
  /* these fields must be filled before initialization */
  png_ihdr_t     ihdr;
  const uint8_t* data;

  /* the following fields must be zero-filled before initialization */
  png_writer_state_t state;

  struct {
    uint8_t* ptr1;
    uint8_t* zero;
    size_t   size;
    size_t   bpp;
    size_t   index;
  } line;

  zng_stream z;

  const char* msg;
} png_writer_t;


HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
bool
png_writer_init(
  png_writer_t* wr);

HEDLEY_NON_NULL(1)
static inline
void
png_writer_deinit(
  png_writer_t* wr);

HEDLEY_NON_NULL(1)
HEDLEY_WARN_UNUSED_RESULT
static inline
size_t
png_writer_write(
  png_writer_t* wr,
  uint8_t*      buf,
  size_t        len);


static inline
void
png_writer_fill_line_(
  png_writer_t* wr);


static inline bool png_writer_init(png_writer_t* wr) {
  assert(png_check_colortype_and_depth(&wr->ihdr));
  assert(wr->ihdr.depth == 8 || wr->ihdr.depth == 16);

  assert(wr->ihdr.colortype   != PNG_INDEXED);
  assert(wr->ihdr.compression == PNG_ZLIB);
  assert(wr->ihdr.filter      == PNG_NO_FILTER);
  assert(wr->ihdr.interlace   == PNG_NO_INTERLACE);

  const size_t bpp = wr->ihdr.depth * png_samples_per_pix(&wr->ihdr);
  wr->line.size = 1+ (bpp*wr->ihdr.width + 7)/8;
  wr->line.bpp  = (bpp + 7)/8;

  if (HEDLEY_UNLIKELY(!upd_malloc(&wr->line.ptr1, wr->line.size))) {
    return false;
  }
  if (HEDLEY_UNLIKELY(!upd_malloc(&wr->line.zero, wr->line.size))) {
    upd_free(&wr->line.ptr1);
    return false;
  }
  memset(wr->line.zero, 0, wr->line.size);

  if (HEDLEY_UNLIKELY(zng_deflateInit(&wr->z, Z_DEFAULT_COMPRESSION) != Z_OK)) {
    upd_free(&wr->line.ptr1);
    upd_free(&wr->line.zero);
    return false;
  }
  return true;
}

static inline void png_writer_deinit(png_writer_t* wr) {
  upd_free(&wr->line.ptr1);
  upd_free(&wr->line.zero);
  zng_deflateEnd(&wr->z);
}

static inline size_t png_writer_write(
    png_writer_t* wr, uint8_t* buf, size_t len) {
  const size_t in = len;

  uint8_t* ptr = NULL;

# define take_(n) do {  \
    ptr = buf;  \
    if (HEDLEY_UNLIKELY(len < (n))) {  \
      goto EXIT;  \
    }  \
    buf += (n);  \
    len -= (n);  \
  } while (0)

BEGIN:
  switch (wr->state) {
  case PNG_WRITER_STATE_IHDR: {
    take_(8+4+4+sizeof(png_ihdr_t)+4);

    *(uint64_t*) ptr = PNG_SIGNATURE;
    ptr += 8;

    *(uint32_t*) ptr = png_u32_endian_inline(sizeof(png_ihdr_t));
    ptr += 4;

    *(uint32_t*) ptr = PNG_IHDR;
    ptr += 4;

    png_ihdr_t ihdr = wr->ihdr;
    png_u32_endian(&ihdr.width);
    png_u32_endian(&ihdr.height);
    *(png_ihdr_t*) ptr = ihdr;
    ptr += sizeof(png_ihdr_t);

    *(uint32_t*) ptr = png_u32_endian_inline(
      ~png_crc(ptr-sizeof(png_ihdr_t)-4, 4+sizeof(png_ihdr_t)));

    wr->state = PNG_WRITER_STATE_IDAT;
  } goto BEGIN;

  case PNG_WRITER_STATE_IDAT: {
    take_(4+4+4);
    uint32_t* chsize = (void*) ptr;
    ptr += 4;
    *(uint32_t*) ptr = PNG_IDAT;
    ptr += 4;

    wr->z.avail_out = len < UINT32_MAX? len: UINT32_MAX;
    wr->z.next_out  = ptr;

    while (wr->z.avail_out && wr->state == PNG_WRITER_STATE_IDAT) {
      bool flush = false;
      if (HEDLEY_UNLIKELY(wr->z.avail_in == 0)) {
        if (HEDLEY_LIKELY(wr->line.index < wr->ihdr.height)) {
          png_writer_fill_line_(wr);
          ++wr->line.index;

          wr->z.avail_in = wr->line.size;
          wr->z.next_in  = wr->line.ptr1;
        } else {
          flush = true;
        }
      }
      const int ret = zng_deflate(&wr->z, flush? Z_FINISH: Z_NO_FLUSH);
      switch (ret) {
      case Z_OK:
        break;
      case Z_STREAM_END:
        wr->state = PNG_WRITER_STATE_IEND;
        break;
      default:
        wr->state = PNG_WRITER_STATE_ERROR;
        wr->msg   = "zlib error";
        return 0;
      }
    }
    buf  = wr->z.next_out;
    len -= buf-ptr;

    *chsize          = png_u32_endian_inline(buf-ptr);
    *(uint32_t*) buf = png_u32_endian_inline(~png_crc(ptr-4, buf-ptr+4));
    buf += 4;
  } goto BEGIN;

  case PNG_WRITER_STATE_IEND:
    take_(4+4+4);
    *(uint32_t*) ptr = 0;
    ptr += 4;
    *(uint32_t*) ptr = PNG_IEND;
    ptr += 4;
    *(uint32_t*) ptr = png_u32_endian_inline(~png_crc(ptr-4, 4));

    wr->state = PNG_WRITER_STATE_DONE;
    goto EXIT;

  default:
    assert(false);
    HEDLEY_UNREACHABLE();
  }

# undef take_

EXIT:
  return in-len;
}


static inline void png_writer_fill_line_(png_writer_t* wr) {
# define FT_ HEDLEY_FALL_THROUGH

  wr->line.ptr1[0] = PNG_PAETH;

  const size_t    linesz = wr->line.size-1;
  const uint8_t*  src    = wr->data + linesz * wr->line.index;
  uint8_t*        dst    = wr->line.ptr1 + 1;

  const uint8_t* up =
    wr->line.index? wr->data + linesz * (wr->line.index-1): wr->line.zero;
  uint8_t left[8]   = {0};
  uint8_t upleft[8] = {0};

  size_t i = (linesz+7)/8, j = 0;
# define proc_(n)  \
    switch (linesz%8) {  \
    case 0: do { *(dst++) = *src - png_paeth(left[j%n], *up, upleft[j%n]); left[j%n] = *src; upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
    case 7:      *(dst++) = *src - png_paeth(left[j%n], *up, upleft[j%n]); left[j%n] = *src; upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
    case 6:      *(dst++) = *src - png_paeth(left[j%n], *up, upleft[j%n]); left[j%n] = *src; upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
    case 5:      *(dst++) = *src - png_paeth(left[j%n], *up, upleft[j%n]); left[j%n] = *src; upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
    case 4:      *(dst++) = *src - png_paeth(left[j%n], *up, upleft[j%n]); left[j%n] = *src; upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
    case 3:      *(dst++) = *src - png_paeth(left[j%n], *up, upleft[j%n]); left[j%n] = *src; upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
    case 2:      *(dst++) = *src - png_paeth(left[j%n], *up, upleft[j%n]); left[j%n] = *src; upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
    case 1:      *(dst++) = *src - png_paeth(left[j%n], *up, upleft[j%n]); left[j%n] = *src; upleft[j%n] = *up; ++j; ++src; ++up; FT_;  \
            } while (--i);  \
    }

  switch (wr->line.bpp) {
  case 1: proc_(1); break;
  case 2: proc_(2); break;
  case 3: proc_(3); break;
  case 4: proc_(4); break;
  case 5: proc_(5); break;
  case 6: proc_(6); break;
  case 7: proc_(7); break;
  case 8: proc_(8); break;
  }

# undef proc_

# undef FT_
}
