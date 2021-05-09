#pragma once

#include <stdbool.h>
#include <stdint.h>


#if !defined(UPD_DECL_FUNC)
# define UPD_DECL_FUNC
#endif


typedef struct upd_iso_t        upd_iso_t;
typedef struct upd_file_t       upd_file_t;
typedef struct upd_file_watch_t upd_file_watch_t;
typedef struct upd_file_lock_t  upd_file_lock_t;
typedef struct upd_driver_t     upd_driver_t;
typedef struct upd_req_t        upd_req_t;

typedef int32_t  upd_iso_status_t;
typedef uint64_t upd_file_id_t;
typedef uint8_t  upd_file_event_t;
typedef uint16_t upd_req_cat_t;
typedef uint32_t upd_req_type_t;

typedef
void
(*upd_file_watch_cb_t)(
  upd_file_watch_t* w);

typedef
void
(*upd_file_lock_cb_t)(
  upd_file_lock_t* l);


/*
 * ---- ISOLATED MACHINE INTERFACE ----
 */
enum {
  /* upd_iso_status_t */
  UPD_ISO_PANIC    = -1,
  UPD_ISO_SHUTDOWN =  0,
  UPD_ISO_REBOOT   =  1,
};

UPD_DECL_FUNC
void*
upd_iso_stack(
  upd_iso_t* iso,
  uint64_t   len);

UPD_DECL_FUNC
void
upd_iso_unstack(
  upd_iso_t* iso,
  void*      ptr);

UPD_DECL_FUNC
uint64_t
upd_iso_now(
  upd_iso_t* iso);

UPD_DECL_FUNC
void
upd_iso_msg(
  upd_iso_t*     iso,
  const uint8_t* msg,
  uint64_t       len);

UPD_DECL_FUNC
void
upd_iso_exit(
  upd_iso_t*       iso,
  upd_iso_status_t status);


/*
 * ---- DRIVER INTERFACE ----
 */
struct upd_driver_t {
  const uint8_t*       name;
  const upd_req_cat_t* cats;

  bool
  (*init)(
    upd_file_t* f);
  void
  (*deinit)(
    upd_file_t* f);
  bool
  (*handle)(
    upd_req_t* req);
};

UPD_DECL_FUNC
bool
upd_driver_register(
  upd_iso_t*          iso,
  const upd_driver_t* driver);

UPD_DECL_FUNC
const upd_driver_t*
upd_driver_lookup(
  upd_iso_t*     iso,
  const uint8_t* name,
  size_t         len);


/*
 * ---- FILE INTERFACE ----
 */
#define UPD_FILE_ID_ROOT 0
#define UPD_FILE_ID_DEV  1
#define UPD_FILE_ID_IO   2
#define UPD_FILE_ID_PROG 3

enum {
  /* upd_file_event_t */
  UPD_FILE_DELETE   = 0x00,
  UPD_FILE_UPDATE   = 0x01,
  UPD_FILE_DELETE_N = 0x10,
  UPD_FILE_UPDATE_N = 0x11,
};

struct upd_file_t {
  upd_iso_t*          iso;
  const upd_driver_t* driver;

  uint8_t* npath;

  upd_file_id_t id;
  uint64_t      refcnt;

  uint64_t last_update;

  void* ctx;
};

struct upd_file_watch_t {
  upd_file_t* file;

  upd_file_watch_cb_t cb;
  void*               udata;

  upd_file_event_t event;
};

struct upd_file_lock_t {
  upd_file_t* file;

  upd_file_lock_cb_t cb;
  void*              udata;

  unsigned ex : 1;
  unsigned ok : 1;
};

UPD_DECL_FUNC
upd_file_t*
upd_file_new(
  upd_iso_t*          iso,
  const upd_driver_t* driver);

UPD_DECL_FUNC
upd_file_t*
upd_file_get(
  upd_iso_t*    iso,
  upd_file_id_t id);

UPD_DECL_FUNC
void
upd_file_ref(
  upd_file_t* file);

UPD_DECL_FUNC
bool
upd_file_unref(
  upd_file_t* file);

UPD_DECL_FUNC
bool
upd_file_watch(
  upd_file_watch_t* w);

UPD_DECL_FUNC
void
upd_file_unwatch(
  upd_file_watch_t* w);

UPD_DECL_FUNC
void
upd_file_trigger(
  upd_file_t*      f,
  upd_file_event_t e);

UPD_DECL_FUNC
bool
upd_file_lock(
  upd_file_lock_t* l);

UPD_DECL_FUNC
void
upd_file_unlock(
  upd_file_lock_t* l);


/*
 * ---- REQUEST INTERFACE ----
 */
#define UPD_REQ_CAT_EACH(f)  \
  f(0x0000, DIR)  \
  f(0x0001, BIN)  \
  f(0x0002, PROG)  \
  f(0x0003, STREAM)

#define UPD_REQ_TYPE_EACH(f)  \
  f(DIR, 0x0000, ACCESS)  \
  f(DIR, 0x0010, LIST)  \
  f(DIR, 0x0020, FIND)  \
  f(DIR, 0x0030, ADD)  \
  f(DIR, 0x0040, RM)  \
\
  f(BIN, 0x0000, ACCESS)  \
  f(BIN, 0x0001, READ)  \
  f(BIN, 0x0002, WRITE)  \
\
  f(PROG, 0x0000, ACCESS)  \
  f(PROG, 0x0010, EXEC)  \
\
  f(STREAM, 0x0000, ACCESS)  \
  f(STREAM, 0x0010, INPUT)  \
  f(STREAM, 0x0020, OUTPUT)

enum {
# define each_(i, N) UPD_REQ_##N = i,
  UPD_REQ_CAT_EACH(each_)
# undef each_

# define each_(C, i, N) UPD_REQ_##C##_##N = (UPD_REQ_##C << 16) | i,
  UPD_REQ_TYPE_EACH(each_)
# undef each_
};

typedef struct upd_req_dir_access_t {
  unsigned list : 1;
  unsigned find : 1;
  unsigned add  : 1;
  unsigned rm   : 1;
} upd_req_dir_access_t;

typedef struct upd_req_dir_entry_t {
  uint8_t*    name;
  size_t      len;
  upd_file_t* file;
  bool        weakref;
} upd_req_dir_entry_t;

typedef struct upd_req_dir_entries_t {
  upd_req_dir_entry_t** p;
  size_t                n;
} upd_req_dir_entries_t;

typedef struct upd_req_bin_access_t {
  unsigned read  : 1;
  unsigned write : 1;
} upd_req_bin_access_t;

typedef struct upd_req_bin_rw_t {
  size_t   offset;
  size_t   size;
  uint8_t* buf;
} upd_req_bin_rw_t;

typedef struct upd_req_prog_access_t {
  unsigned exec : 1;
} upd_req_prog_access_t;

typedef struct upd_req_stream_access_t {
  unsigned input  : 1;
  unsigned output : 1;
} upd_req_stream_access_t;

typedef struct upd_req_stream_io_t {
  size_t   size;
  uint8_t* buf;
} upd_req_stream_io_t;

struct upd_req_t {
  upd_file_t* file;

  upd_req_type_t type;

  void* udata;
  void
  (*cb)(
    upd_req_t* req);

  union {
    union {
      upd_req_dir_access_t  access;
      upd_req_dir_entry_t   entry;
      upd_req_dir_entries_t entries;
    } dir;
    union {
      upd_req_bin_access_t access;
      upd_req_bin_rw_t     rw;
    } bin;
    union {
      upd_req_prog_access_t access;
      upd_file_t*           exec;
    } prog;
    union {
      upd_req_stream_access_t access;
      upd_req_stream_io_t     io;
    } stream;
  };
};

UPD_DECL_FUNC
bool
upd_req(
  upd_req_t* req);
