local R_ = {}


---- ## std.lambda ## ----
local std_lambda = {
  -- information level --
  INFO = "info",   -- suggestion
                   --   e.g.) performance issue
  WARN = "warn",   -- reccommendation
                   --   e.g.) result may be broken (professionals can ignore)
  ERR  = "error",  -- execution is impossible
}
std_lambda.__index = std_lambda

function R_.lambda(ctx)
  local L = {
    ctx    = ctx,
    upk    = std.msgpack.unpacker(),
    input  = {},
    output = {},
  }
  return std.lua.setMetatable(L, std_lambda)
end

function std_lambda:openInput(name, trait, desc)
  self.input[name] = {
    value = nil,
    trait = trait,
    desc  = desc,
  }
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "openInput",
      param     = {
        name  = name,
        desc  = desc,
        trait = trait.param,
        value = trait.param.def,
      },
    }))
end
function std_lambda:closeInput(name)
  self.input[name] = nil
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "closeInput",
      param     = {
        name = name,
      },
    }))
end

function std_lambda:openOutput(name, trait, desc)
  self.output[name] = {
    value = nil,
    trait = trait,
  }
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "openOutput",
      param     = {
        name  = name,
        desc  = desc,
        trait = trait.param,
      },
    }))
end
function std_lambda:closeOutput(name)
  self.output[name] = nil
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "closeOutput",
      param     = {
        name = name,
      },
    }))
end

function std_lambda:getInput(name)
  local i = self.input[name];
  return i.value or i.trait.default
end
function std_lambda:setOutput(name, value)
  local o = self.output[name];
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "output",
      param     = {
        name  = name,
        value = o.trait:serialize(value),
      },
    }))
end

function std_lambda:inform(lv, msg, type, name)
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "inform",
      param     = {
        level  = lv,
        msg    = msg,
        target = {
          type = type,
          name = name,
        },
      },
    }))
end
function std_lambda:informInput(lv, name, msg)
  self:inform(lv, msg, "input", name)
end
function std_lambda:informOutput(lv, name, msg)
  self:inform(lv, msg, "output", name)
end

function std_lambda:done()
  self.ctx.send(std.msgpack.pack({ result = true, }))
end

function std_lambda:handle()
  function err_(msg)
    self.ctx.send(std.msgpack.pack({success = false, msg = msg}))
  end
  function proc_(req)
    if not req.command then
      return false  -- we don't care response from clients
    end
    if req.interface and (req.interface ~= "lambda") then
      err_("unknown interface")
      return false
    end

    local switch = {}
    function switch.input()
      local i = self.input[req.param.name];
      if i == nil then
        err_("unknown name")
        return false
      end

      local v = req.param.value
      if not i.trait:test(v) then
        err_("invalid value")
        return false
      end
      i.value = i.trait:deserialize(v)
      self:done()
      return false
    end
    function switch.exec()
      return true
    end

    local dlg = switch[req.command]
    if dlg then
      return dlg()
    else
      err_("unknown command")
      return false
    end
  end

  while true do
    local cnt = 0
    while cnt == 0 do
      cnt = self.upk:unpack(self.ctx.recvBlocked())
      if not cnt then
        return false
      end
    end
    for i = 1, cnt do
      local req = self.upk:pop()
      if proc_(req) then
        return true
      end
    end
  end
end


---- ## std.msgpack.* ## ----
function R_.mpk_pack(...)
  local pk = std.msgpack.packer()
  pk:pack(...)
  return pk:take()
end

function R_.mpk_unpack(...)
  local upk = std.msgpack.unpacker()
  local cnt = upk:unpack(...)
  return upk:pop(cnt)
end


---- ## std.trait.* ## ----
local std_trait_number = {}
std_trait_number.__index = std_trait_number

function R_.trait_number(args)
  local T = {
    param = {
      max  = args.max,
      min  = args.min,
      step = args.step,
      def  = args.def,
    },
    onTest = args.onTest,
  }
  return std.lua.setMetatable(T, std_trait_number)
end

function std_trait_number:test(v)
  local p = self.param
  if not v then
    if not p.def then
      return false
    end
    return self.onTest(p.def)
  end

  if not std.lua.isNumber(v) then
    return false
  end

  if p.min and v < p.min then
    return false
  end
  if p.max and v > p.max then
    return false
  end
  return self.onTest(v)
end

function std_trait_number:serialize(v)
  return v
end

function std_trait_number:deserialize(v)
  return v or self.def
end


return R_
