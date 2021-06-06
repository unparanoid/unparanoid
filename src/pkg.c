#include "common.h"


#define FORBIDDEN_CHARS_ "/<>:\"\\|_*"


typedef struct mkdir_t_    mkdir_t_;
typedef struct download_t_ download_t_;

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
  size_t  wrote;

  uv_fs_t fsreq;

  unsigned open  : 1;
  unsigned abort : 1;
  unsigned ok    : 1;

  void
  (*cb)(
    download_t_* md);
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
void
download_finalize_(
  download_t_* d,
  bool         ok);


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
pkg_abort_fs_cleanup_cb_(
  uv_fs_t* fsreq);


static
void
download_open_cb_(
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
download_write_cb_(
  uv_fs_t* fsreq);

static
void
download_complete_cb_(
  CURL* curl,
  void* udata);

static
void
download_close_cb_(
  uv_fs_t* fsreq);


static
void
mkdir_stat_cb_(
  uv_fs_t* fsreq);

static
void
mkdir_mkdir_cb_(
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


static void pkg_logf_(upd_pkg_install_t* inst, const char* fmt, ...) {
  upd_iso_msgf(inst->iso, "pkg error: ");

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
  if (HEDLEY_UNLIKELY(curl_url_get(u, CURLUPART_HOST, (char**) &host, 0))) {
    pkg_logf_(inst, "failed to allocate string for url host");
    curl_url_cleanup(u);
    return false;
  }
  curl_url_cleanup(u);

  /* validate hostname */
  const size_t hostlen = utf8size_lazy(host);
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
    curl_free(host);
    pkg_logf_(inst, "too long pkg name");
    return false;
  }
  utf8ncpy(name, inst->name, inst->namelen);
  name[inst->namelen] = 0;

  const size_t used =
    cwk_path_join((char*) host, (char*) name, (char*) str, len);
  curl_free(host);
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
  upd_pkg_t* pkg = inst->pkg;

  /* remove archive file */
  uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
  if (HEDLEY_LIKELY(fsreq != NULL)) {
    *fsreq = (uv_fs_t) { .data = iso, };
    const int unlink = uv_fs_unlink(
      &iso->loop, fsreq, (char*) pkg->npath_archive, pkg_abort_fs_cleanup_cb_);
    if (HEDLEY_UNLIKELY(unlink < 0)) {
      upd_iso_unstack(iso, fsreq);
    }
  }

  if (HEDLEY_LIKELY(ok)) {
    inst->pkg->install = NULL;
    inst->state = UPD_PKG_INSTALL_DONE;

  } else {
    upd_array_find_and_remove(&inst->iso->pkgs, inst->pkg);
    inst->state = UPD_PKG_INSTALL_ABORTED;

    static const char* msg =
      "the broken pkg remaining on native filesystem because of low memory, "
      "please delete '%s' by yourself :(";

    /* delete a dir of the broken pkg */
    uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
    if (HEDLEY_UNLIKELY(fsreq == NULL)) {
      upd_free(&inst->pkg);
      pkg_logf_(inst, msg, inst->pkg->npath);
      goto EXIT;
    }
    *fsreq = (uv_fs_t) { .data = iso, };

    const int rmdir = uv_fs_rmdir(
      &iso->loop, fsreq, (char*) inst->pkg->npath, pkg_abort_fs_cleanup_cb_);
    upd_free(&inst->pkg);
    if (HEDLEY_LIKELY(rmdir >= 0)) {

    } else if (rmdir == UV_ENOENT) {
      upd_iso_unstack(iso, fsreq);

    } else {
      pkg_logf_(inst, msg, inst->pkg->npath);
      upd_iso_unstack(iso, fsreq);
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


static void download_finalize_(download_t_* d, bool ok) {
  upd_iso_t* iso  = d->iso;
  CURL*      curl = d->curl;
  uv_file    file = d->file;
  const bool open = d->open;

  d->ok = ok;
  d->cb(d);

  if (HEDLEY_LIKELY(curl)) {
    curl_easy_cleanup(curl);
  }
  if (HEDLEY_LIKELY(open)) {
    uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
    if (HEDLEY_UNLIKELY(fsreq == NULL)) {
      return;
    }
    *fsreq = (uv_fs_t) { .data = iso, };

    const int close = uv_fs_close(&iso->loop, fsreq, file, download_close_cb_);
    if (HEDLEY_LIKELY(0 > close)) {
      upd_iso_unstack(iso, fsreq);
      return;
    }
  }
}


static void pkg_mkdir_cb_(mkdir_t_* md) {
  upd_pkg_install_t* inst = md->inst;
  upd_iso_t*         iso  = inst->iso;

  const bool exists  = md->exists;
  const bool created = md->created;
  upd_iso_unstack(iso, md);

  if (HEDLEY_LIKELY(exists)) {
    /* TODO: configure the pkg */
    pkg_finalize_install_(inst, true);
    return;
  }
  if (HEDLEY_UNLIKELY(!created)) {
    pkg_logf_(inst, "mkdir failure");
    goto ABORT;
  }

  download_t_* d = upd_iso_stack(iso, sizeof(*d));
  if (HEDLEY_UNLIKELY(d == NULL)) {
    pkg_logf_(inst, "download context allocation failure");
    goto ABORT;
  }
  *d = (download_t_) {
    .iso   = iso,
    .inst  = inst,
    .fsreq = { .data = d, },
    .cb    = pkg_download_cb_,
  };
  const int open = uv_fs_open(
    &iso->loop,
    &d->fsreq,
    (char*) inst->pkg->npath_archive,
    O_WRONLY | O_CREAT,
    0600,
    download_open_cb_);
  if (HEDLEY_UNLIKELY(0 > open)) {
    upd_iso_unstack(iso, d);
    pkg_logf_(inst, "archive file open failure");
    goto ABORT;
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
    pkg_logf_(inst, "pkg download failure");
    goto ABORT;
  }

  /* TODO */
  pkg_logf_(inst, "%s -> %s", inst->pkg->url, inst->pkg->npath);
  pkg_finalize_install_(inst, true);
  return;

ABORT:
  pkg_finalize_install_(inst, false);
}

static void pkg_abort_fs_cleanup_cb_(uv_fs_t* fsreq) {
  upd_iso_t* iso = fsreq->data;
  uv_fs_req_cleanup(fsreq);
  upd_iso_unstack(iso, fsreq);
}


static void download_open_cb_(uv_fs_t* fsreq) {
  download_t_*       d    = fsreq->data;
  upd_pkg_install_t* inst = d->inst;
  upd_iso_t*         iso  = inst->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    pkg_logf_(inst, "archive file open failure");
    goto ABORT;
  }
  d->open = true;
  d->file = result;

  CURL* curl = curl_easy_init();
  if (HEDLEY_UNLIKELY(curl == NULL)) {
    pkg_logf_(inst, "curl init failure");
    goto ABORT;
  }
  d->curl = curl;
  const bool setopt =
    !curl_easy_setopt(curl, CURLOPT_NOPROGRESS, true) &&
    !curl_easy_setopt(curl, CURLOPT_NOSIGNAL, true) &&
    !curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_recv_cb_) &&
    !curl_easy_setopt(curl, CURLOPT_WRITEDATA, d) &&
    !curl_easy_setopt(curl, CURLOPT_URL, inst->pkg->url) &&
    !curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, (long) inst->verify_ssl);
  if (HEDLEY_UNLIKELY(!setopt)) {
    pkg_logf_(inst, "curl setopt failure");
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(!upd_iso_curl_perform(iso, curl, download_complete_cb_, d))) {
    pkg_logf_(inst, "curl perform failure");
    goto ABORT;
  }
  return;

ABORT:
  download_finalize_(d, false);
}

static size_t download_recv_cb_(
    void* data, size_t size, size_t nmemb, void* udata) {
  download_t_*       d    = udata;
  upd_pkg_install_t* inst = d->inst;
  upd_iso_t*         iso  = d->iso;

  const size_t realsize = size * nmemb;
  if (HEDLEY_UNLIKELY(realsize == 0)) {
    return 0;
  }
  if (HEDLEY_UNLIKELY(d->abort)) {
    return 0;
  }

  const uv_buf_t buf = uv_buf_init(data, realsize);

  const int write = uv_fs_write(
    &iso->loop, &d->fsreq, d->file, &buf, 1, d->wrote, download_write_cb_);
  if (HEDLEY_UNLIKELY(0 > write)) {
    pkg_logf_(inst, "failed to write downloaded data");
    return 0;
  }
  d->wrote += realsize;
  curl_easy_pause(d->curl, CURLPAUSE_RECV);
  return realsize;
}

static void download_write_cb_(uv_fs_t* fsreq) {
  download_t_*       d    = fsreq->data;
  upd_pkg_install_t* inst = d->inst;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    pkg_logf_(inst, "failed to write downloaded data");
    d->abort = true;
  }
  curl_easy_pause(d->curl, 0);
}

static void download_complete_cb_(CURL* curl, void* udata) {
  download_t_*       d    = udata;
  upd_pkg_install_t* inst = d->inst;

  long res;
  const bool getinfo =
    !curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res);

  if (HEDLEY_UNLIKELY(!getinfo)) {
    pkg_logf_(inst, "curl getinfo failure");
    goto ABORT;
  }
  if (HEDLEY_UNLIKELY(res < 200 || 300 <= res)) {
    pkg_logf_(inst, "http error (%ld)", res);
    goto ABORT;
  }
  download_finalize_(d, true);
  return;

ABORT:
  download_finalize_(d, false);
}

static void download_close_cb_(uv_fs_t* fsreq) {
  upd_iso_t* iso = fsreq->data;
  uv_fs_req_cleanup(fsreq);
  upd_iso_unstack(iso, fsreq);
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
