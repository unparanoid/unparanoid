#pragma once

#include <stdbool.h>
#include <stdint.h>


#if !defined(UPD_DECL_FUNC)
# define UPD_DECL_FUNC
#endif


typedef struct upd_iso_t    upd_iso_t;
typedef struct upd_file_t   upd_file_t;
typedef struct upd_driver_t upd_driver_t;
typedef struct upd_req_t    upd_req_t;

typedef int32_t  upd_iso_status_t;
typedef uint64_t upd_file_id_t;
typedef uint8_t  upd_file_event_t;
typedef uint16_t upd_req_cat_t;
typedef uint32_t upd_req_type_t;

typedef
void
(*upd_file_watch_cb_t)(
  upd_file_t*      f,
  upd_file_event_t e,
  void*            udata);


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

  void
  (*init)(
    upd_iso_t*  iso,
    upd_file_t* f);
  void
  (*deinit)(
    upd_iso_t*  iso,
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
#define UPD_FILE_INVALID UINT64_MAX
#define UPD_FILE_ROOT    0

enum {
  /* upd_file_event_t */
  UPD_FILE_DELETE = 0x00,
  UPD_FILE_UPDATE = 0x01,
};

struct upd_file_t {
  upd_iso_t*          iso;
  const upd_driver_t* driver;

  upd_file_id_t id;
  uint64_t      refcnt;
  void*         ctx;
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
void
upd_file_unref(
  upd_file_t* file);

UPD_DECL_FUNC
bool
upd_file_watch(
  upd_file_t*         f,
  uint64_t*           id,
  upd_file_watch_cb_t cb,
  void*               udata);

UPD_DECL_FUNC
bool
upd_file_unwatch(
  upd_file_t* f,
  uint64_t    id);


/*
 * ---- REQUEST INTERFACE ----
 */
#define UPD_REQ_CAT_EACH(f)  \
  f(0x0000, DIR)

#define UPD_REQ_TYPE_EACH(f)  \
  f(DIR, 0x0000, LIST)  \
  f(DIR, 0x0010, FIND)  \
  f(DIR, 0x0020, ADD)  \
  f(DIR, 0x0030, RM)

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

struct upd_req_t {
  upd_iso_t*  iso;
  upd_file_t* file;

  upd_req_type_t type;

  void* udata;
  void
  (*callback)(
    upd_req_t* req);

  union {
    union {
      upd_req_dir_access_t access;
    } dir;
  };
};

UPD_DECL_FUNC
bool
upd_req(
  upd_req_t* req);
