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
#include <lauxlib.h>
#include <lua.h>
#include <mimetype.h>
#include <picohttpparser.h>
#include <sha1.h>
#include <utf8.h>
#include <uv.h>
#include <yaml.h>
#include <wsock.h>

#if UPD_USE_VALGRIND
# include <valgrind.h>
#endif

#define Z_PREFIX_SET
#include <zlib.h>
#undef Z_PREFIX_SET


#define UPD_DECL_FUNC static inline
#include <libupd.h>
#undef UPD_DECL_FUNC

#include <libupd/array.h>
#include <libupd/buf.h>
#include <libupd/memory.h>
#include <libupd/path.h>
#include <libupd/pathfind.h>


typedef struct upd_cli_t upd_cli_t;
typedef struct upd_pkg_t upd_pkg_t;
typedef struct upd_srv_t upd_srv_t;

#if defined(UPD_TEST)
# include "test.h"
#endif

#include "iso.h"

#include "cli.h"
#include "config.h"
#include "driver.h"
#include "file.h"
#include "pkg.h"
#include "srv.h"
