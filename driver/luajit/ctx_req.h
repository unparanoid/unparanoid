#pragma once


static void req_cb_(upd_req_t* req) {
  lj_promise_t* pro = req->udata;
  upd_file_t*   stf = pro->stream;
  upd_iso_t*    iso = stf->iso;
  lj_stream_t*  st  = stf->ctx;
  lua_State*    L   = st->L;

  if (HEDLEY_UNLIKELY(req->result != UPD_REQ_OK)) {
    upd_iso_unstack(iso, req);
    lj_promise_finalize(pro, false);
    return;
  }

  switch (req->type) {
  case UPD_REQ_DIR_LIST:
    lua_createtable(L, 1, 0);
    {
      lua_createtable(L, 0, req->dir.entries.n);
      for (size_t i = 0; i < req->dir.entries.n; ++i) {
        upd_req_dir_entry_t* e = req->dir.entries.p[i];
        lua_pushlstring(L, (char*) e->name, e->len);
        lj_file_new(stf, e->file);
        lua_settable(L, -3);
      }
      lua_rawseti(L, -2, 1);
    }
    pro->registry.result = luaL_ref(L, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_DIR_FIND:
  case UPD_REQ_DIR_ADD:
  case UPD_REQ_DIR_NEW:
  case UPD_REQ_DIR_NEWDIR:
  case UPD_REQ_DIR_RM:
    lj_file_new(stf, req->dir.entry.file);
    pro->registry.result = luaL_ref(L, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_PROG_EXEC:
    lj_file_new(stf, req->prog.exec);
    pro->registry.result = luaL_ref(L, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_STREAM_READ:
  case UPD_REQ_DSTREAM_READ:
    lua_pushlstring(L, (char*) req->stream.io.buf, req->stream.io.size);
    pro->registry.result = luaL_ref(L, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_STREAM_WRITE:
  case UPD_REQ_DSTREAM_WRITE:
    lua_pushinteger(L, req->stream.io.size);
    pro->registry.result = luaL_ref(L, LUA_REGISTRYINDEX);
    break;

  case UPD_REQ_STREAM_TRUNCATE:
    break;

  case UPD_REQ_TENSOR_META:
    lua_createtable(L, 1, 0);
    {
      lua_createtable(L, 0, 2);
      {
        const uint32_t rank = req->tensor.meta.rank;
        lua_pushinteger(L, rank);
        lua_setfield(L, -2, "rank");

        lua_createtable(L, rank, 0);
        for (size_t i = 0; i < rank; ++i) {
          lua_pushinteger(L, req->tensor.meta.reso[i]);
          lua_rawseti(L, -2, i+1);
        }
        lua_setfield(L, -2, "reso");
      }
      lua_rawseti(L, -2, 1);
    }
    pro->registry.result = luaL_ref(L, LUA_REGISTRYINDEX);
    break;

  default:
    upd_iso_unstack(iso, req);
    lj_promise_finalize(pro, false);
    luaL_error(L, "not implemented req type");
  }
  upd_iso_unstack(iso, req);
  lj_promise_finalize(pro, true);
}
static int req_(lua_State* L) {
  upd_file_t*          stf  = lua_touserdata(L, lua_upvalueindex(1));
  const upd_req_type_t type = lua_tointeger(L, lua_upvalueindex(2));

  upd_file_lock_t* k = *(void**) luaL_checkudata(L, 1, "Lock");
  if (HEDLEY_UNLIKELY(k == NULL)) {
    return luaL_error(L, "first arg must be a Lock object");
  }

  upd_file_t* f   = k->file;
  upd_iso_t*  iso = f->iso;

  switch (type) {
  case UPD_REQ_DIR_ADD:
  case UPD_REQ_DIR_NEW:
  case UPD_REQ_DIR_NEWDIR:
  case UPD_REQ_DIR_RM:
  case UPD_REQ_STREAM_WRITE:
  case UPD_REQ_STREAM_TRUNCATE:
  case UPD_REQ_DSTREAM_READ:
  case UPD_REQ_DSTREAM_WRITE:
  case UPD_REQ_TENSOR_META:
  case UPD_REQ_TENSOR_DATA:
  case UPD_REQ_TENSOR_FLUSH:
    if (HEDLEY_UNLIKELY(!k->ex)) {
      return luaL_error(L, "requires exlock");
    }
    break;
  default:
    break;
  }

  lj_promise_t* pro   = lj_promise_new(stf);
  size_t        index = lua_gettop(L);
  upd_req_t*    req   = NULL;
  bool          ok    = false;

  switch (type) {
  case UPD_REQ_DIR_LIST:
  case UPD_REQ_PROG_EXEC:
  case UPD_REQ_TENSOR_META:
    ok = upd_req_with_dup(&(upd_req_t) {
        .file  = f,
        .type  = type,
        .udata = pro,
        .cb    = req_cb_,
      });
    break;

  case UPD_REQ_DIR_NEW:
  case UPD_REQ_DIR_NEWDIR:
  case UPD_REQ_DIR_FIND: {
    size_t len;
    const char* name = luaL_checklstring(L, 2, &len);

    req = upd_iso_stack(iso, sizeof(*req)+len);
    if (HEDLEY_UNLIKELY(req == NULL)) {
      break;
    }
    *req = (upd_req_t) {
      .file = f,
      .type = type,
      .dir  = { .entry = {
        .name = utf8ncpy(req+1, name, len),
        .len  = len,
      }, },
      .udata = pro,
      .cb    = req_cb_,
    };
    ok = upd_req(req);
  } break;

  case UPD_REQ_DIR_ADD: {
    upd_file_t* child = luaL_checkudata(L, 2, "File");

    size_t len;
    const char* name = luaL_checklstring(L, 3, &len);

    req = upd_iso_stack(iso, sizeof(*req)+len);
    if (HEDLEY_UNLIKELY(req == NULL)) {
      break;
    }
    *req = (upd_req_t) {
      .file = f,
      .type = type,
      .dir  = { .entry = {
        .name = utf8ncpy(req+1, name, len),
        .len  = len,
        .file = child,
      }, },
      .udata = pro,
      .cb    = req_cb_,
    };
    ok = upd_req(req);
  } break;

  case UPD_REQ_DIR_RM: {
    upd_file_t* child = NULL;
    size_t      len   = 0;
    const char* name  = NULL;

    if (lua_isstring(L, 2)) {
      name = luaL_checklstring(L, 2, &len);
    } else {
      child = luaL_checkudata(L, 2, "File");
    }

    req = upd_iso_stack(iso, sizeof(*req)+len);
    if (HEDLEY_UNLIKELY(req == NULL)) {
      break;
    }
    *req = (upd_req_t) {
      .file = f,
      .type = type,
      .dir  = { .entry = {
        .name = utf8ncpy(req+1, name, len),
        .len  = len,
        .file = child,
      }, },
      .udata = pro,
      .cb    = req_cb_,
    };
    ok = upd_req(req);
  } break;

  case UPD_REQ_STREAM_READ:
  case UPD_REQ_DSTREAM_READ: {
    lua_Integer len = INT64_MAX;
    if (lua_isnumber(L, 2)) {
      len = lua_tointeger(L, 2);
    }
    const lua_Integer offset = lua_tointeger(L, 3);
    if (HEDLEY_UNLIKELY(len < 0 || offset < 0)) {
      return luaL_error(L, "negative range");
    }
    ok = upd_req_with_dup(&(upd_req_t) {
        .file = f,
        .type = type,
        .stream  = { .io = {
          .size   = len,
          .offset = offset,
        }, },
        .udata = pro,
        .cb    = req_cb_,
      });
  } break;

  case UPD_REQ_STREAM_WRITE:
  case UPD_REQ_DSTREAM_WRITE: {
    size_t len;
    const char* buf = luaL_checklstring(L, 2, &len);

    const lua_Integer offset = lua_tointeger(L, 3);
    if (HEDLEY_UNLIKELY(offset < 0)) {
      return luaL_error(L, "negative offset");
    }

    req = upd_iso_stack(iso, sizeof(*req)+len);
    if (HEDLEY_UNLIKELY(req == NULL)) {
      break;
    }
    *req = (upd_req_t) {
      .file = f,
      .type = type,
      .stream  = { .io = {
        .buf    = memcpy(req+1, buf, len),
        .size   = len,
        .offset = offset,
      }, },
      .udata = pro,
      .cb    = req_cb_,
    };
    ok = upd_req(req);
  } break;

  case UPD_REQ_STREAM_TRUNCATE: {
    const lua_Integer len = luaL_checkinteger(L, 2);
    if (HEDLEY_UNLIKELY(len < 0)) {
      return luaL_error(L, "negative length");
    }
    ok = upd_req_with_dup(&(upd_req_t) {
        .file = f,
        .type = type,
        .stream  = { .io = {
          .size = len,
        }, },
        .udata = pro,
        .cb    = req_cb_,
      });
  } break;

  default:
    luaL_error(L, "unknown req type");
  }

  if (HEDLEY_UNLIKELY(!ok)) {
    if (HEDLEY_LIKELY(req)) {
      upd_iso_unstack(iso, req);
    }
    lj_promise_finalize(pro, false);
  }
  lua_pushvalue(L, index);
  return 1;
}

static void req_create_(lua_State* L) {
  const int stf = lua_gettop(L);

  lua_createtable(L, 0, 0);
  {
    lua_createtable(L, 0, 0);
    {
      lua_createtable(L, 0, 0);
      {
#       define begin_() do {  \
          lua_createtable(L, 0, 0);  \
          lua_createtable(L, 0, 0);  \
          lua_createtable(L, 0, 0);  \
        } while (0)

#       define end_(n) do {  \
          lua_setfield(L, -2, "__index");  \
          lua_pushcfunction(L, lj_lua_immutable_newindex);  \
          lua_setfield(L, -2, "__newindex");  \
          lua_setmetatable(L, -2);  \
          lua_setfield(L, -2, n);  \
        } while (0)

#       define set_(n, N) do {  \
          lua_pushvalue(L, stf);  \
          lua_pushinteger(L, N);  \
          lua_pushcclosure(L, req_, 2);  \
          lua_setfield(L, -2, n);  \
        } while (0)

        begin_();
        {
          set_("list",   UPD_REQ_DIR_LIST);
          set_("find",   UPD_REQ_DIR_FIND);
          set_("add",    UPD_REQ_DIR_ADD);
          set_("new",    UPD_REQ_DIR_NEW);
          set_("newdir", UPD_REQ_DIR_NEWDIR);
          set_("rm",     UPD_REQ_DIR_RM);
        }
        end_("dir");

        begin_();
        {
          set_("exec", UPD_REQ_PROG_EXEC);
        }
        end_("prog");

        begin_();
        {
          set_("read",     UPD_REQ_STREAM_READ);
          set_("write",    UPD_REQ_STREAM_WRITE);
          set_("truncate", UPD_REQ_STREAM_TRUNCATE);
        }
        end_("stream");

        begin_();
        {
          set_("read",  UPD_REQ_DSTREAM_READ);
          set_("write", UPD_REQ_DSTREAM_WRITE);
        }
        end_("dstream");

        begin_();
        {
          set_("meta",  UPD_REQ_TENSOR_META);
        }
        end_("tensor");

#       undef set_
#       undef end_
#       undef begin_
      }
      lua_setfield(L, -2, "__index");

      lua_pushcfunction(L, lj_lua_immutable_newindex);
      lua_setfield(L, -2, "__newindex");
    }
    lua_setmetatable(L, -2);
  }
  lua_remove(L, stf);
}
