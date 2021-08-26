local R_ = {}


---- ## std.msgpack.* ## ----
function R_.mpk_pack(...)
  local pk = std.msgpack.packer();
  pk:pack(...);
  return pk:take();
end

function R_.mpk_unpack(...)
  local upk = std.msgpack.unpacker();
  local cnt = upk:unpack(...);
  return upk:pop(cnt);
end


return R_
