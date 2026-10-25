-- 两两组合
local function mix(x, y) return x * 3 + y end
local xs, ys = {}, {}
for i = 0, 19999 do xs[#xs + 1] = (i * 7919) % 10007 end
for i = 0, 18999 do ys[#ys + 1] = (i * 31) % 997 end
local total = 0
for r = 0, 9 do
  local z = {}
  local n = math.min(#xs, #ys)
  for i = 1, n do z[i] = mix(xs[i], ys[i]) end
  total = total + #z + z[1] + z[#z]
end
print(total)
