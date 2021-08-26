local Number = {}
Number.__index = Number

function Number.new(args)
  local T = {
    param = {
      max  = args.max,
      min  = args.min,
      step = args.step,
      def  = args.def,
    },
    onTest = args.onTest,
  }
  return std.lua.setMetatable(T, Number)
end

function Number:test(v)
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

function Number:serialize(v)
  return v
end

function Number:deserialize(v)
  return v or self.def
end


return {
  Number = Number,
}
