local Protocol = {
  -- information level --
  INFO = "info",   -- suggestion
                   --   e.g.) performance issue
  WARN = "warn",   -- reccommendation
                   --   e.g.) result may be broken (professionals can ignore)
  ERR  = "error",  -- execution is impossible
};
Protocol.__index = Protocol;

function Protocol.new(ctx)
  local L = {
    ctx    = ctx,
    upk    = std.msgpack.unpacker(),
    input  = {},
    output = {},
  };
  return std.lua.setMetatable(L, Protocol);
end

function Protocol:openInput(name, trait, desc)
  self.input[name] = {
    value = nil,
    trait = trait,
    desc  = desc,
  };
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "openInput",
      param     = {
        name  = name,
        desc  = desc,
        trait = trait.param,
        value = trait.param.def,
      },
    }));
end
function Protocol:closeInput(name)
  self.input[name] = nil;
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "closeInput",
      param     = {
        name = name,
      },
    }));
end

function Protocol:openOutput(name, trait, desc)
  self.output[name] = {
    value = nil,
    trait = trait,
  };
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "openOutput",
      param     = {
        name  = name,
        desc  = desc,
        trait = trait.param,
      },
    }));
end
function Protocol:closeOutput(name)
  self.output[name] = nil;
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "closeOutput",
      param     = {
        name = name,
      },
    }));
end

function Protocol:getInput(name)
  local i = self.input[name];
  return i.value or i.trait.default;
end
function Protocol:setOutput(name, value)
  local o = self.output[name];
  self.ctx.send(std.msgpack.pack({
      interface = "lambda",
      command   = "output",
      param     = {
        name  = name,
        value = o.trait:serialize(value),
      },
    }));
end

function Protocol:inform(lv, msg, type, name)
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
    }));
end
function Protocol:informInput(lv, name, msg)
  self:inform(lv, msg, "input", name);
end
function Protocol:informOutput(lv, name, msg)
  self:inform(lv, msg, "output", name);
end

function Protocol:done()
  self.ctx.send(std.msgpack.pack({ result = true, }));
end

function Protocol:handle()
  function err_(msg)
    self.ctx.send(std.msgpack.pack({success = false, msg = msg}));
  end
  function proc_(req)
    if not req.command then
      return false;  -- response from clients is ignored in this protocol
    end
    if req.interface and (req.interface ~= "lambda") then
      err_("unknown interface")
      return false;
    end

    local switch = {};
    function switch.input()
      local i = self.input[req.param.name];
      if i == nil then
        err_("unknown name");
        return false;
      end

      local v = req.param.value;
      if not i.trait:test(v) then
        err_("invalid value");
        return false;
      end
      i.value = i.trait:deserialize(v);
      self:done();
      return false;
    end
    function switch.exec()
      return true;
    end

    local dlg = switch[req.command];
    if dlg then
      return dlg();
    else
      err_("unknown command");
      return false;
    end
  end

  local receiver = self.ctx.recv();
  while true do
    local cnt = 0;
    while cnt == 0 do
      cnt = self.upk:unpack(receiver:await());
      if not cnt then
        return false;
      end
    end
    for i = 1, cnt do
      local req = self.upk:pop();
      if proc_(req) then
        return true;
      end
    end
  end
end


return {
  Protocol = Protocol,
};
