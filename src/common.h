#pragma once

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <base64.h>
#include <curl/curl.h>
#include <cwalk.h>
#include <hedley.h>
#include <mimetype.h>
#include <sha1.h>
#include <utf8.h>
#include <uv.h>
#include <yaml.h>
#include <wsock.h>
#include <zlib-ng.h>

#if UPD_USE_VALGRIND
# include <valgrind.h>
#endif


#define UPD_DECL_FUNC static inline
#include <libupd.h>
#undef UPD_DECL_FUNC

#include <libupd/array.h>
#include <libupd/buf.h>
#include <libupd/memory.h>
#include <libupd/path.h>
#include <libupd/pathfind.h>


typedef struct upd_pkg_t upd_pkg_t;

#include "iso.h"

#include "config.h"
#include "driver.h"
#include "file.h"
#include "pkg.h"
