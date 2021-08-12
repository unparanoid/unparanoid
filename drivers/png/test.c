#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "reader.h"
#include "writer.h"


int main(int argc, char** argv) {
  assert(argc == 3);

  FILE* fp = fopen(argv[1], "rb");
  assert(fp);

  png_reader_t re = {0};
  assert(png_reader_init(&re));

  uint8_t buf[1024*1024];
  size_t  off = 0;
  while (!feof(fp) && !(re.state & PNG_READER_STATE_DONE)) {
    const size_t size = fread(buf+off, 1, sizeof(buf)-off, fp);
    const size_t used = png_reader_consume(&re, buf, off+size);

    off = off + size - used;
    memcpy(buf, buf+used, off);
  }
  fclose(fp);

  if (re.state == PNG_READER_STATE_DONE) {
    printf("read ok\n");
  } else {
    printf("read error: %s\n", re.msg);
    return 1;
  }

  uint8_t* pixbuf = png_reader_deinit(&re);

  png_writer_t wr = {
    .ihdr = {
      .width     = re.ihdr.width,
      .height    = re.ihdr.height,
      .depth     = re.pix.depth*8,
      .colortype = re.ihdr.colortype,
    },
    .data = pixbuf,
  };
  assert(png_writer_init(&wr));

  fp = fopen(argv[2], "wb");
  assert(fp);

  while (!(wr.state & PNG_WRITER_STATE_DONE)) {
    const size_t wrote = png_writer_write(&wr, buf, sizeof(buf));
    assert(wrote > 0);

    assert(fwrite(buf, wrote, 1, fp) == 1);
  }
  fclose(fp);
  if (wr.state == PNG_WRITER_STATE_DONE) {
    printf("write ok\n");
  } else {
    printf("write error: %s\n", wr.msg);
    return 1;
  }

  png_writer_deinit(&wr);
  return 0;
}
