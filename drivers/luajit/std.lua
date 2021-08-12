return {
  mpk_pack = function (...)
    local pk = std.msgpack.packer()
    pk.pack(...)
    return pk.buffer
  end,

  mpk_unpack = function (...)
    local upk = std.msgpack.unpacker()
    upk.unpack(...)
    if upk.broken or upk.count == 0 then
      return nil
    end

    local function pop(i)
      i = i+1
      return upk.pop(), i < upk.count and pop(i) or nil
    end
    return pop(0)
  end,
}
