-- Go风格时长格式化
local function frac(v, prec)
  local out, printed = "", false
  for _ = 1, prec do
    local digit = v % 10
    printed = printed or digit ~= 0
    if printed then out = tostring(digit) .. out end
    v = v // 10
  end
  if printed then out = "." .. out end
  return out, v
end
local function go_format(d)
  local neg = d < 0
  local u = neg and -d or d
  local s
  if u < 1000000000 then
    if u == 0 then return "0s" end
    local prec, unit
    if u < 1000 then prec, unit = 0, "ns" elseif u < 1000000 then prec, unit = 3, "\u{b5}s" else prec, unit = 6, "ms" end
    local f; f, u = frac(u, prec); s = tostring(u) .. f .. unit
  else
    local f; f, u = frac(u, 9); s = tostring(u % 60) .. f .. "s"; u = u // 60
    if u > 0 then s = tostring(u % 60) .. "m" .. s; u = u // 60; if u > 0 then s = tostring(u) .. "h" .. s end end
  end
  return (neg and "-" or "") .. s
end
local total = 0
for i = 0, 99999 do
  local d = (i * 7919 * 104729) % 100000000000000 - 5000000000000
  total = total + #go_format(d)
end
print(total)
