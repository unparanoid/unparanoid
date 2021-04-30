#pragma once

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hedley.h>
#include <utf8.h>
#include <uv.h>
#include <yaml.h>


#define UPD_DECL_FUNC static inline
#include <libupd.h>
#undef UPD_DECL_FUNC


#include "memory.h"

#include "array.h"
#include "buf.h"


typedef struct upd_srv_t upd_srv_t;
typedef struct upd_cli_t upd_cli_t;

#if defined(UPD_TEST)
# include "test.h"
#endif

#include "iso.h"

#include "cli.h"
#include "driver.h"
#include "file.h"
#include "req.h"
#include "srv.h"
