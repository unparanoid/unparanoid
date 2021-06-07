#include "common.h"


#define FORBIDDEN_CHARS_ "/<>:\"\\|_*"

#define TAR_CHUNK_        (1024*256)
#define TAR_NAME_SIZE_    100
#define TAR_SIZE_OFFSET_  124
#define TAR_SIZE_SIZE_    12
#define TAR_TYPE_OFFSET_  156
#define TAR_MAGIC_        "ustar  "
#define TAR_MAGIC_SIZE_   6
#define TAR_MAGIC_OFFSET_ 257
#define TAR_HEADER_SIZE_  512


typedef struct mkdir_t_    mkdir_t_;
typedef struct download_t_ download_t_;
typedef struct rmdir_t_    rmdir_t_;

struct mkdir_t_ {
  uv_fs_t fsreq;

  upd_pkg_install_t* inst;

  uint8_t path[UPD_PATH_MAX];

  struct cwk_segment seg;

  unsigned last_seg : 1;
  unsigned exists   : 1;
  unsigned created  : 1;

  void
  (*cb)(
    mkdir_t_* md);
};

struct download_t_ {
  upd_iso_t*         iso;
  upd_pkg_install_t* inst;

  CURL*   curl;
  uv_file file;

  z_stream z;
  uv_fs_t  fsreq;

  size_t refcnt;

  unsigned ok         : 1;
  unsigned stream_end : 1;

  uint8_t* recvbuf;

  struct {
    uint8_t* chunk;
    size_t   recv;
    size_t   parsed;

    size_t pad;

    uv_file f;
    size_t  fsize;
    size_t  frecv;
  } tar;

  void
  (*cb)(
    download_t_* md);
};

struct rmdir_t_ {
  uv_fs_t    fsreq;
  upd_iso_t* iso;
  size_t     refcnt;

  rmdir_t_* parent;

  void
  (*cb)(
    rmdir_t_* r);
};


HEDLEY_PRINTF_FORMAT(2, 3)
static
void
pkg_logf_(
  upd_pkg_install_t* inst,
  const char*        fmt,
  ...);

static
bool
pkg_build_url_(
  upd_pkg_install_t* inst,
  uint8_t*           str,
  size_t             len);

static
bool
pkg_build_nrpath_(
  upd_pkg_install_t* inst,
  uint8_t*           str,
  size_t             len);

static
upd_pkg_t*
pkg_new_(
  upd_pkg_install_t* inst);

static
void
pkg_finalize_install_(
  upd_pkg_install_t* inst,
  bool               ok);


static
void
mkdir_(
  mkdir_t_* mkdir);

static
void
mkdir_down_(
  mkdir_t_* mkdir);


static
bool
download_inflate_(
  download_t_* d);

static
bool
download_parse_tar_(
  download_t_* d);

static
void
download_unref_(
  download_t_* d);


static
bool
rmdir_with_dup_(
  const rmdir_t_* src,
  const uint8_t*  path);

static
void
rmdir_unref_(
  rmdir_t_* r);


static
void
pkg_mkdir_cb_(
  mkdir_t_* md);

static
void
pkg_download_cb_(
  download_t_* d);

static
void
pkg_rmdir_cb_(
  rmdir_t_* r);


static
void
mkdir_stat_cb_(
  uv_fs_t* fsreq);

static
void
mkdir_mkdir_cb_(
  uv_fs_t* fsreq);


static
size_t
download_recv_cb_(
  void*  data,
  size_t size,
  size_t nmemb,
  void*  udata);

static
void
download_open_cb_(
  uv_fs_t* fsreq);

static
void
download_write_cb_(
  uv_fs_t* fsreq);

static
void
download_close_cb_(
  uv_fs_t* fsreq);

static
void
download_mkdir_cb_(
  uv_fs_t* fsreq);

static
void
download_complete_cb_(
  CURL* curl,
  void* udata);


static
void
rmdir_scandir_cb_(
  uv_fs_t* fsreq);

static
void
rmdir_unlink_cb_(
  uv_fs_t* fsreq);

static
void
rmdir_rmdir_cb_(
  uv_fs_t* fsreq);

static
void
rmdir_sub_cb_(
  rmdir_t_* r);


static
void
file_unstack_cb_(
  uv_fs_t* fsreq);


bool upd_pkg_install(upd_pkg_install_t* inst) {
  upd_iso_t* iso = inst->iso;

  inst->pkg = NULL;
  if (HEDLEY_UNLIKELY(!pkg_new_(inst))) {
    return false;
  }
  for (size_t i = 0; i < iso->pkgs.n; ++i) {
    upd_pkg_t* pkg = iso->pkgs.p[i];
    if (HEDLEY_UNLIKELY(utf8cmp(pkg->nrpath, inst->pkg->nrpath) == 0)) {
      upd_free(&inst->pkg);
      pkg_logf_(inst, "pkg name conflict");
      return false;
    }
  }
  if (HEDLEY_UNLIKELY(!upd_array_insert(&iso->pkgs, inst->pkg, SIZE_MAX))) {
    upd_free(&inst->pkg);
    pkg_logf_(inst, "pkg insertion failure");
    return false;
  }

  inst->state = UPD_PKG_INSTALL_MKDIR;

  mkdir_t_* md = upd_iso_stack(iso, sizeof(*md));
  if (HEDLEY_UNLIKELY(md == NULL)) {
    pkg_finalize_install_(inst, false);
    pkg_logf_(inst, "pkg insertion failure");
    return false;
  }
  *md = (mkdir_t_) {
    .inst = inst,
    .cb   = pkg_mkdir_cb_,
  };
  mkdir_(md);
  return true;
}

void upd_pkg_abort_install(upd_pkg_install_t* inst) {
  inst->abort = true;
}


static void pkg_logf_(upd_pkg_install_t* inst, const char* fmt, ...) {
  upd_iso_msgf(inst->iso, "pkg: ");

  va_list args;
  va_start(args, fmt);
  upd_iso_msgfv(inst->iso, fmt, args);
  va_end(args);

  if (HEDLEY_LIKELY(inst->pkg)) {
    upd_iso_msgf(inst->iso, " (%s)\n", inst->pkg->nrpath);
  } else {
    upd_iso_msgf(inst->iso, " (%.*s)\n", (int) inst->namelen, inst->name);
  }
}

static bool pkg_build_url_(upd_pkg_install_t* inst, uint8_t* str, size_t len) {
  size_t prefix = 0;
  while (inst->src[prefix] != '$') {
    if (HEDLEY_UNLIKELY(++prefix >= inst->srclen)) {
      pkg_logf_(inst, "invalid source url ($ is not found)");
      return false;
    }
  }
  const size_t postfix = inst->srclen - prefix - 1;

  const size_t whole = prefix + inst->namelen + postfix + 1;
  if (HEDLEY_UNLIKELY(len < whole)) {
    pkg_logf_(inst, "too long url");
    return false;
  }

  utf8ncpy(str, inst->src, prefix);
  str += prefix;

  utf8ncpy(str, inst->name, inst->namelen);
  str += inst->namelen;

  utf8ncpy(str, inst->src+prefix+1, postfix);
  str += postfix;

  *str = 0;
  return true;
}

static bool pkg_build_nrpath_(upd_pkg_install_t* inst, uint8_t* str, size_t len) {
  uint8_t src[UPD_PATH_MAX];
  if (HEDLEY_UNLIKELY(inst->srclen >= sizeof(src))) {
    pkg_logf_(inst, "too long url");
    return false;
  }
  utf8ncpy(src, inst->src, inst->srclen);
  src[inst->srclen] = 0;

  /* parse url and get hostname */
  CURLU* u = curl_url();
  if (HEDLEY_UNLIKELY(u == NULL)) {
    pkg_logf_(inst, "failed to allocate url parser");
    return false;
  }
  if (HEDLEY_UNLIKELY(curl_url_set(u, CURLUPART_URL, (char*) src, 0))) {
    curl_url_cleanup(u);
    pkg_logf_(inst, "failed to parse url");
    return false;
  }
  uint8_t* host = NULL;
  curl_url_get(u, CURLUPART_HOST, (char**) &host, 0);
  curl_url_cleanup(u);

  /* validate hostname */
  const size_t hostlen = host? utf8size_lazy(host): 0;
  for (size_t i = 0; i < hostlen; ++i) {
    if (HEDLEY_UNLIKELY(!isprint(host[i]))) {
      curl_free(host);
      pkg_logf_(inst, "host name must consist of ASCII-printable character");
      return false;
    }
    if (HEDLEY_UNLIKELY(utf8chr(FORBIDDEN_CHARS_, host[i]))) {
      curl_free(host);
      pkg_logf_(inst, "host name contains forbidden char: %c", host[i]);
      return false;
    }
  }

  /* join pkgname into the path */
  uint8_t name[256];
  if (HEDLEY_UNLIKELY(inst->namelen+1 > sizeof(name))) {
    if (host) curl_free(host);
    pkg_logf_(inst, "too long pkg name");
    return false;
  }
  utf8ncpy(name, inst->name, inst->namelen);
  name[inst->namelen] = 0;

  const size_t used = cwk_path_join(
    host? (char*) host: "_local", (char*) name, (char*) str, len);
  if (host) curl_free(host);
  if (HEDLEY_UNLIKELY(len < used+1)) {
    pkg_logf_(inst, "too long path");
    return false;
  }
  len -= used;
  return true;
}

static upd_pkg_t* pkg_new_(upd_pkg_install_t* inst) {
  upd_iso_t* iso = inst->iso;

  assert(inst->pkg == NULL);

  uint8_t url[1024];
  uint8_t nrpath[UPD_PATH_MAX];
  const bool build_str =
    pkg_build_url_(inst, url, sizeof(url)) &&
    pkg_build_nrpath_(inst, nrpath, sizeof(nrpath));
  if (HEDLEY_UNLIKELY(!build_str)) {
    return NULL;
  }

  uint8_t npath[UPD_PATH_MAX];
  const size_t npathlen = cwk_path_join(
    (char*) iso->path.pkg, (char*) nrpath, (char*) npath, sizeof(npath));
  if (HEDLEY_UNLIKELY(sizeof(npath) < 1+npathlen)) {
    return NULL;
  }

  uint8_t npatha[UPD_PATH_MAX];
  const size_t npathalen = cwk_path_change_extension(
    (char*) npath, ".temp_", (char*) npatha, sizeof(npatha));
  if (HEDLEY_UNLIKELY(sizeof(npatha) < 1+npathalen)) {
    return NULL;
  }

  const size_t namelen   = inst->namelen;
  const size_t urllen    = utf8size_lazy(url);
  const size_t nrpathlen = utf8size_lazy(nrpath);

  const size_t whole = sizeof(*inst->pkg) +
    namelen+1 + urllen+1 + nrpathlen+1 + npathlen+1 + npathalen+1;
  if (HEDLEY_UNLIKELY(!upd_malloc(&inst->pkg, whole))) {
    return NULL;
  }
  upd_pkg_t* pkg = inst->pkg;

  *pkg = (upd_pkg_t) {
    .install = inst,
  };
  pkg->name = (uint8_t*) (pkg+1);
  utf8ncpy(pkg->name, inst->name, namelen);
  pkg->name[namelen] = 0;

  pkg->url = pkg->name + namelen+1;
  utf8cpy(pkg->url, url);

  pkg->nrpath = pkg->url + urllen+1;
  utf8cpy(pkg->nrpath, nrpath);

  pkg->npath = pkg->nrpath + nrpathlen+1;
  utf8cpy(pkg->npath, npath);

  pkg->npath_archive = pkg->npath + npathlen+1;
  utf8cpy(pkg->npath_archive, npatha);
  return pkg;
}

static void pkg_finalize_install_(upd_pkg_install_t* inst, bool ok) {
  upd_iso_t* iso = inst->iso;
  inst->pkg->install = NULL;

  if (HEDLEY_LIKELY(ok)) {
    inst->state = UPD_PKG_INSTALL_DONE;

  } else {
    inst->state = UPD_PKG_INSTALL_ABORTED;

    const bool rmdir = rmdir_with_dup_(&(rmdir_t_) {
        .iso = iso,
        .cb  = pkg_rmdir_cb_,
      }, inst->pkg->npath);
    if (HEDLEY_UNLIKELY(!rmdir)) {
      pkg_logf_(inst,
        "the broken pkg remaining on native filesystem because of low memory, "
        "please delete '%s' by yourself :(", inst->pkg->npath);
      goto EXIT;
    }
  }

EXIT:
  inst->cb(inst);
}


static void mkdir_(mkdir_t_* md) {
  upd_pkg_install_t* inst = md->inst;
  upd_iso_t*         iso  = inst->iso;

  md->exists   = false;
  md->created  = false;
  md->last_seg = false;

  const bool seg =
    cwk_path_get_first_segment((char*) inst->pkg->nrpath, &md->seg);
  if (HEDLEY_UNLIKELY(!seg)) {
    md->cb(md);
    return;
  }

  utf8cpy(md->path, iso->path.pkg);

  const int stat = uv_fs_stat(
    &iso->loop, &md->fsreq, (char*) inst->pkg->npath, mkdir_stat_cb_);
  if (HEDLEY_UNLIKELY(stat >= 0)) {
    return;
  }

  mkdir_down_(md);
}

static void mkdir_down_(mkdir_t_* md) {
  upd_pkg_install_t* inst = md->inst;
  upd_iso_t*         iso  = inst->iso;

  if (HEDLEY_UNLIKELY(md->last_seg)) {
    md->created = true;
    md->cb(md);
    return;
  }

  uint8_t seg[UPD_PATH_MAX];
  utf8ncpy(seg, md->seg.begin, md->seg.size);
  seg[md->seg.size] = 0;

  /*  md->path will never be overflown because the complete path
   * stored in inst->npath is shorter than UPD_PATH_MAX. */
  cwk_path_join((char*) md->path, (char*) seg, (char*) md->path, UPD_PATH_MAX);

  const int mkdir = uv_fs_mkdir(
    &iso->loop, &md->fsreq, (char*) md->path, 0755, mkdir_mkdir_cb_);

  md->last_seg = !cwk_path_get_next_segment(&md->seg);
  if (HEDLEY_LIKELY(mkdir >= 0)) {

  } else if (mkdir == UV_EEXIST) {
    mkdir_down_(md);

  } else {
    pkg_logf_(inst, "mkdir '%s' failure", md->path);
    md->cb(md);
    return;
  }
}


static bool download_inflate_(download_t_* d) {
  upd_pkg_install_t* inst = d->inst;
  z_stream*          z    = &d->z;

  if (HEDLEY_UNLIKELY(z->avail_in == 0 || inst->abort)) {
    curl_easy_pause(d->curl, 0);
    return true;
  }

  z->avail_out = TAR_CHUNK_ - d->tar.recv;
  z->next_out  = d->tar.chunk + d->tar.recv;

  const int ret = inflate(z, Z_NO_FLUSH);
  switch (ret) {
  case Z_NEED_DICT:
  case Z_DATA_ERROR:
    pkg_logf_(inst, "invalid gzip format");
    return false;
  case Z_MEM_ERROR:
    pkg_logf_(inst, "zlib memory error");
    return false;
  case Z_STREAM_END:
    d->stream_end = z->avail_out;
  }
  d->tar.recv   = TAR_CHUNK_ - z->avail_out;
  d->tar.parsed = 0;
  return download_parse_tar_(d);
}

static bool download_parse_tar_(download_t_* d) {
  upd_pkg_install_t* inst = d->inst;
  upd_iso_t*         iso  = d->iso;

  if (HEDLEY_UNLIKELY(inst->abort)) {
    return download_inflate_(d);
  }

  if (HEDLEY_UNLIKELY(d->tar.recv <= d->tar.parsed)) {
    d->tar.parsed = 0;
    d->tar.recv   = 0;
    return download_inflate_(d);
  }
  const uint8_t* chunk   = d->tar.chunk + d->tar.parsed;
  const size_t   chunksz = d->tar.recv - d->tar.parsed;

  const size_t pad = d->tar.pad > chunksz? chunksz: d->tar.pad;
  if (HEDLEY_UNLIKELY(pad)) {
    d->tar.parsed += pad;
    d->tar.pad    -= pad;
    return download_parse_tar_(d);
  }

  /* recv file contents */
  if (HEDLEY_UNLIKELY(d->tar.frecv < d->tar.fsize)) {
    const size_t rem = d->tar.fsize - d->tar.frecv;
    const size_t sz  = rem < chunksz? rem: chunksz;

    const uv_buf_t buf = uv_buf_init((char*) chunk, sz);

    ++d->refcnt;
    const int write = uv_fs_write(&iso->loop,
      &d->fsreq, d->tar.f, &buf, 1, d->tar.frecv, download_write_cb_);
    if (HEDLEY_UNLIKELY(0 > write)) {
      download_unref_(d);
      pkg_logf_(inst, "file write failure");
      return false;
    }
    return true;
  }

  /* empty name means EOF in tar format */
  if (HEDLEY_UNLIKELY(chunk[0] == 0)) {
    d->stream_end = true;  /* tar is now completed, so we can finish downloading */
    return download_inflate_(d);
  }
  if (HEDLEY_UNLIKELY(chunksz < TAR_HEADER_SIZE_)) {
    memmove(d->tar.chunk, chunk, chunksz);
    d->tar.recv = chunksz;
    return download_inflate_(d);
  }
  d->tar.parsed += TAR_HEADER_SIZE_;

  uint8_t name[TAR_NAME_SIZE_+1];
  utf8ncpy(name, chunk, TAR_NAME_SIZE_);
  name[TAR_NAME_SIZE_] = 0;

  const uint8_t* ssize = chunk + TAR_SIZE_OFFSET_;
  size_t size = 0;
  for (size_t i = 0; i < TAR_SIZE_SIZE_; ++i) {
    const uint8_t c = ssize[i];
    if (HEDLEY_UNLIKELY(c == 0)) {
      break;
    }
    if (HEDLEY_UNLIKELY(!isdigit(c))) {
      pkg_logf_(inst, "invalid size specification in tar header");
      return false;
    }
    size *= 8;
    size += c-'0';
  }

  const uint8_t* magic = chunk + TAR_MAGIC_OFFSET_;
  if (HEDLEY_UNLIKELY(utf8ncmp(magic, TAR_MAGIC_, TAR_MAGIC_SIZE_))) {
    pkg_logf_(inst, "unknown tar magic '%.*s'", TAR_MAGIC_SIZE_, magic);
    return false;
  }

  uint8_t path[UPD_PATH_MAX];
  const size_t join = cwk_path_join(
    (char*) inst->pkg->npath, (char*) name, (char*) path, UPD_PATH_MAX);
  if (HEDLEY_UNLIKELY(join >= UPD_PATH_MAX)) {
    pkg_logf_(inst, "tar has a file with too long path");
    return false;
  }

  const uint8_t typeflag = chunk[TAR_TYPE_OFFSET_];
  switch (typeflag) {
  case 0:
  case '0': {  /* file */
    pkg_logf_(inst, "file: %.100s (%zu)", name, size);
    d->tar.frecv = 0;
    d->tar.fsize = size;

    ++d->refcnt;
    const int open = uv_fs_open(
      &iso->loop,
      &d->fsreq,
      (char*) path,
      O_WRONLY | O_CREAT | O_EXCL,
      0600,
      download_open_cb_);
    if (HEDLEY_UNLIKELY(0 > open)) {
      download_unref_(d);
      pkg_logf_(inst, "file open failure (%s)", uv_err_name(open));
      return false;
    }
  } return true;

  case '5': {  /* directory */
    pkg_logf_(inst, "dir: %.100s", name);

    ++d->refcnt;
    const int mkdir = uv_fs_mkdir(
      &iso->loop,
      &d->fsreq,
      (char*) path,
      0755,
      download_mkdir_cb_);
    if (HEDLEY_UNLIKELY(0 > mkdir)) {
      download_unref_(d);
      pkg_logf_(inst, "mkdir failure (%s)", uv_err_name(mkdir));
      return false;
    }
  } return true;

  default:
    pkg_logf_(inst, "unknown tar typeflag: %c", typeflag);
    return download_parse_tar_(d);
  }
}

static void download_unref_(download_t_* d) {
  upd_iso_t*         iso  = d->iso;
  upd_pkg_install_t* inst = d->inst;

  if (HEDLEY_UNLIKELY(--d->refcnt == 0)) {
    upd_free(&d->recvbuf);
    upd_free(&d->tar.chunk);
    curl_easy_cleanup(d->curl);
    inflateEnd(&d->z);

    if (HEDLEY_UNLIKELY(d->tar.frecv < d->tar.fsize)) {
      pkg_logf_(inst, "stream ends unexpectedly");

      uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
      if (HEDLEY_LIKELY(fsreq != NULL)) {
        *fsreq = (uv_fs_t) { .data = iso, };
        const int close =
          uv_fs_close(&iso->loop, fsreq, d->tar.f, file_unstack_cb_);
        if (HEDLEY_UNLIKELY(0 > close)) {
          upd_iso_unstack(iso, fsreq);
          pkg_logf_(inst,
            "broken file close failure (%s)", uv_err_name(close));
        }
      }
    } else {
      d->ok = true;
    }
    d->cb(d);
  }
}


static bool rmdir_with_dup_(const rmdir_t_* src, const uint8_t* path) {
  upd_iso_t* iso = src->iso;

  if (HEDLEY_UNLIKELY(utf8size_lazy(path) >= UPD_PATH_MAX)) {
    return false;
  }

  rmdir_t_* r = upd_iso_stack(iso, sizeof(*r));
  if (HEDLEY_UNLIKELY(r == NULL)) {
    return false;
  }
  *r = *src;
  r->refcnt = 1;

  uv_fs_req_cleanup(&r->fsreq);
  const int scandir = uv_fs_scandir(
    &iso->loop, &r->fsreq, (char*) path, 0, rmdir_scandir_cb_);
  if (HEDLEY_UNLIKELY(0 > scandir)) {
    upd_iso_unstack(iso, r);
    return false;
  }
  return true;
}

static void rmdir_unref_(rmdir_t_* r) {
  upd_iso_t* iso = r->iso;

  assert(r->refcnt);
  if (HEDLEY_LIKELY(--r->refcnt)) {
    return;
  }

  uint8_t path[UPD_PATH_MAX];
  utf8cpy(path, r->fsreq.path);
  uv_fs_req_cleanup(&r->fsreq);

  const int err = uv_fs_rmdir(
    &iso->loop, &r->fsreq, (char*) path, rmdir_rmdir_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    upd_iso_unstack(iso, r);
    upd_iso_msgf(iso, "rmdir: rmdir error (%s)\n", uv_err_name(err));
    return;
  }
}


static void pkg_mkdir_cb_(mkdir_t_* md) {
  upd_pkg_install_t* inst = md->inst;
  upd_iso_t*         iso  = inst->iso;
  upd_pkg_t*         pkg  = inst->pkg;

  const bool exists  = md->exists;
  const bool created = md->created;
  upd_iso_unstack(iso, md);

  if (HEDLEY_LIKELY(exists)) {
    /* TODO: configure the pkg */
    pkg_finalize_install_(inst, true);
    return;
  }
  pkg_logf_(inst, "installing...");

  if (HEDLEY_UNLIKELY(!created)) {
    pkg_logf_(inst, "mkdir failure");
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(inst->abort)) {
    pkg_logf_(inst, "aborting installation");
    goto ABORT;
  }

  download_t_* d = upd_iso_stack(iso, sizeof(*d));
  if (HEDLEY_UNLIKELY(d == NULL)) {
    pkg_logf_(inst, "download context allocation failure");
    goto ABORT;
  }
  *d = (download_t_) {
    .iso  = iso,
    .inst = inst,
    .z    = {
      .zalloc   = Z_NULL,
      .zfree    = Z_NULL,
      .opaque   = Z_NULL,
      .avail_in = 0,
      .next_in  = Z_NULL,
    },
    .fsreq = {
      .data = d,
    },
    .refcnt = 1,
    .cb     = pkg_download_cb_,
  };

  if (HEDLEY_UNLIKELY(!upd_malloc(&d->tar.chunk, TAR_CHUNK_))) {
    upd_iso_unstack(iso, d);
    pkg_logf_(inst, "chunk allocation failure");
    goto ABORT;
  }

  CURL* curl = curl_easy_init();
  if (HEDLEY_UNLIKELY(curl == NULL)) {
    upd_free(&d->tar.chunk);
    upd_iso_unstack(iso, d);
    pkg_logf_(inst, "curl init failure");
    goto ABORT;
  }
  d->curl = curl;

  if (HEDLEY_UNLIKELY(z_inflateInit2(&d->z, 15+32) != Z_OK)) {
    curl_easy_cleanup(curl);
    upd_free(&d->tar.chunk);
    upd_iso_unstack(iso, d);
    pkg_logf_(inst, "zlib stream init failure");
    goto ABORT;
  }

  const bool setopt =
    !curl_easy_setopt(curl, CURLOPT_NOPROGRESS, true) &&
    !curl_easy_setopt(curl, CURLOPT_NOSIGNAL, true) &&
    !curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_recv_cb_) &&
    !curl_easy_setopt(curl, CURLOPT_WRITEDATA, d) &&
    !curl_easy_setopt(curl, CURLOPT_URL, pkg->url) &&
    !curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, (long) inst->verify_ssl);
  if (HEDLEY_UNLIKELY(!setopt)) {
    pkg_logf_(inst, "curl setopt failure");
    download_unref_(curl);
    return;
  }
  if (HEDLEY_UNLIKELY(!upd_iso_curl_perform(iso, curl, download_complete_cb_, d))) {
    pkg_logf_(inst, "curl perform failure");
    download_unref_(curl);
    return;
  }
  return;

ABORT:
  pkg_finalize_install_(inst, false);
}

static void pkg_download_cb_(download_t_* d) {
  upd_pkg_install_t* inst = d->inst;
  upd_iso_t*         iso  = inst->iso;

  const bool downloaded = d->ok;
  upd_iso_unstack(iso, d);

  if (HEDLEY_UNLIKELY(!downloaded)) {
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(inst->abort)) {
    pkg_logf_(inst, "aborting installation");
    goto ABORT;
  }

  /* TODO */
  pkg_logf_(inst, "%s -> %s", inst->pkg->url, inst->pkg->npath);
  pkg_logf_(inst, "successfully installed!");
  pkg_finalize_install_(inst, false);
  return;

ABORT:
  pkg_finalize_install_(inst, false);
}

static void pkg_rmdir_cb_(rmdir_t_* r) {
  upd_iso_t* iso = r->iso;
  upd_iso_unstack(iso, r);
}


static void mkdir_stat_cb_(uv_fs_t* fsreq) {
  mkdir_t_* md = (void*) fsreq;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result >= 0)) {
    md->exists = true;
    md->cb(md);
    return;
  }
  mkdir_down_(md);
}

static void mkdir_mkdir_cb_(uv_fs_t* fsreq) {
  mkdir_t_*          md   = (void*) fsreq;
  upd_pkg_install_t* inst = md->inst;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0 && result != UV_EEXIST)) {
    pkg_logf_(inst, "mkdir '%s' failure", md->path);
    md->cb(md);
    return;
  }
  mkdir_down_(md);
}


static size_t download_recv_cb_(
    void* data, size_t size, size_t nmemb, void* udata) {
  download_t_*       d    = udata;
  upd_pkg_install_t* inst = d->inst;
  z_stream*          z    = &d->z;

  const size_t realsize = size * nmemb;
  if (HEDLEY_UNLIKELY(realsize == 0)) {
    return 0;
  }
  if (HEDLEY_UNLIKELY(inst->abort || d->stream_end)) {
    return 0;
  }

  if (HEDLEY_UNLIKELY(curl_easy_pause(d->curl, CURLPAUSE_RECV))) {
    pkg_logf_(inst, "curl pause failure");
    return 0;
  }
  if (HEDLEY_UNLIKELY(!upd_malloc(&d->recvbuf, realsize))) {
    pkg_logf_(inst, "curl recv buffer allocation failure");
    return 0;
  }
  memcpy(d->recvbuf, data, realsize);

  z->avail_in = realsize;
  z->next_in  = d->recvbuf;
  if (HEDLEY_UNLIKELY(!download_inflate_(d))) {
    inst->abort = true;
    return 0;
  }
  return realsize;
}

static void download_open_cb_(uv_fs_t* fsreq) {
  download_t_*       d    = fsreq->data;
  upd_pkg_install_t* inst = d->inst;
  upd_iso_t*         iso  = d->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    pkg_logf_(inst, "file open failure (%s)", uv_err_name(result));
    inst->abort = true;
    goto EXIT;
  }
  d->tar.f = result;

  if (HEDLEY_UNLIKELY(d->tar.fsize == 0)) {
    const int close =
      uv_fs_close(&iso->loop, &d->fsreq, d->tar.f, download_close_cb_);
    if (HEDLEY_UNLIKELY(0 > close)) {
      pkg_logf_(inst, "empty file close failure (%s)", uv_err_name(close));
      goto EXIT;
    }
    return;
  }

EXIT:
  if (HEDLEY_UNLIKELY(!download_parse_tar_(d))) {
    inst->abort = true;
  }
  download_unref_(d);
}

static void download_write_cb_(uv_fs_t* fsreq) {
  download_t_*       d    = fsreq->data;
  upd_pkg_install_t* inst = d->inst;
  upd_iso_t*         iso  = d->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    pkg_logf_(inst, "file write failure");
    inst->abort = true;
    goto CLOSE;
  }
  d->tar.parsed += result;
  d->tar.frecv  += result;
  if (HEDLEY_UNLIKELY(d->tar.frecv >= d->tar.fsize)) {
    d->tar.pad = (512-d->tar.fsize%512)%512;
    goto CLOSE;
  }
  goto EXIT;

CLOSE:
  d->tar.frecv = 0;
  d->tar.fsize = 0;
  const int close =
    uv_fs_close(&iso->loop, &d->fsreq, d->tar.f, download_close_cb_);
  if (HEDLEY_UNLIKELY(0 > close)) {
    pkg_logf_(inst, "written file close failure (%s)", uv_err_name(close));
    goto EXIT;
  }
  return;

EXIT:
  if (HEDLEY_UNLIKELY(!download_parse_tar_(d))) {
    inst->abort = true;
  }
  download_unref_(d);
}

static void download_close_cb_(uv_fs_t* fsreq) {
  download_t_*       d    = fsreq->data;
  upd_pkg_install_t* inst = d->inst;

  if (HEDLEY_UNLIKELY(fsreq->result < 0)) {
    pkg_logf_(inst, "file close failure (%s)", uv_err_name(fsreq->result));
  }
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(!download_parse_tar_(d))) {
    d->inst->abort = true;
  }
  download_unref_(d);
}

static void download_mkdir_cb_(uv_fs_t* fsreq) {
  download_t_*       d    = fsreq->data;
  upd_pkg_install_t* inst = d->inst;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    pkg_logf_(inst, "mkdir failure");
    inst->abort = true;
    return;
  }
  if (HEDLEY_UNLIKELY(!download_parse_tar_(d))) {
    inst->abort = true;
  }
  download_unref_(d);
}

static void download_complete_cb_(CURL* curl, void* udata) {
  download_t_*       d    = udata;
  upd_pkg_install_t* inst = d->inst;

  uint8_t* scheme;
  long     res;

  const bool getinfo =
    !curl_easy_getinfo(curl, CURLINFO_SCHEME, (char**) &scheme) &&
    !curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res);
  if (HEDLEY_UNLIKELY(!getinfo)) {
    pkg_logf_(inst, "curl getinfo failure");
    goto EXIT;
  }
  if (HEDLEY_UNLIKELY(inst->abort)) {
    pkg_logf_(inst, "download is aborted");
    goto EXIT;
  }

EXIT:
  download_unref_(d);
}


static void rmdir_scandir_cb_(uv_fs_t* fsreq) {
  rmdir_t_*  r   = (void*) fsreq;
  upd_iso_t* iso = r->iso;

  uint8_t self[UPD_PATH_MAX];
  utf8cpy(self, fsreq->path);

  uv_dirent_t e;
  while (0 <= uv_fs_scandir_next(fsreq, &e)) {
    uint8_t path[UPD_PATH_MAX];

    const size_t join = cwk_path_join(
      (char*) self, e.name, (char*) path, UPD_PATH_MAX);
    if (HEDLEY_UNLIKELY(join >= UPD_PATH_MAX)) {
      upd_iso_msgf(iso, "rmdir: too long path\n");
      continue;
    }

    if (e.type == UV_DIRENT_DIR) {
      ++r->refcnt;
      const bool sub = rmdir_with_dup_(&(rmdir_t_) {
          .iso    = iso,
          .parent = r,
          .cb     = rmdir_sub_cb_,
        }, path);
      if (HEDLEY_UNLIKELY(!sub)) {
        upd_iso_msgf(iso, "rmdir: subreq failure\n");
        continue;
      }
    } else {
      uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
      if (HEDLEY_UNLIKELY(fsreq == NULL)) {
        upd_iso_msgf(iso, "rmdir: subreq allocation failure\n");
        continue;
      }
      *fsreq = (uv_fs_t) { .data = r, };

      ++r->refcnt;
      const int unlink = uv_fs_unlink(
        &iso->loop, fsreq, (char*) path, rmdir_unlink_cb_);
      if (HEDLEY_UNLIKELY(0 > unlink)) {
        upd_iso_unstack(iso, fsreq);
        rmdir_unref_(r);
        upd_iso_msgf(iso, "rmdir: unlink failure (%s)\n", uv_err_name(unlink));
        continue;
      }
    }
  }
  rmdir_unref_(r);
}

static void rmdir_unlink_cb_(uv_fs_t* fsreq) {
  rmdir_t_*  r   = fsreq->data;
  upd_iso_t* iso = r->iso;

  if (HEDLEY_UNLIKELY(fsreq->result < 0)) {
    upd_iso_msgf(iso, "rmdir: unlink error (%s)\n", uv_err_name(fsreq->result));
  }
  uv_fs_req_cleanup(fsreq);
  upd_iso_unstack(iso, fsreq);
  rmdir_unref_(r);
}

static void rmdir_rmdir_cb_(uv_fs_t* fsreq) {
  rmdir_t_*  r   = (void*) fsreq;
  upd_iso_t* iso = r->iso;

  if (HEDLEY_UNLIKELY(fsreq->result < 0)) {
    upd_iso_msgf(iso, "rmdir: rmdir error (%s)\n", uv_err_name(fsreq->result));
  }
  uv_fs_req_cleanup(fsreq);
  r->cb(r);
}

static void rmdir_sub_cb_(rmdir_t_* r) {
  upd_iso_t* iso = r->iso;

  if (HEDLEY_UNLIKELY(r->parent)) {
    rmdir_unref_(r->parent);
  }
  upd_iso_unstack(iso, r);
}


static void file_unstack_cb_(uv_fs_t* fsreq) {
  upd_iso_t* iso = fsreq->data;
  uv_fs_req_cleanup(fsreq);
  upd_iso_unstack(iso, fsreq);
}
