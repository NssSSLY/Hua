-- top-k：排序后取前100
local xs, x = {}, 1
for i = 1, 200000 do x = (x * 1103515245 + 12345) % 2147483648; xs[i] = x % 10000000 end
local total = 0
for r = 0, 4 do
  local c = {}
  for i = 1, #xs do c[i] = xs[i] end
  table.sort(c)
  local s = 0
  for i = 1, 100 do s = s + c[i] end
  total = total + s + r
end
print(total)
