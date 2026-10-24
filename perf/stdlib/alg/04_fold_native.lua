-- 归约
local function step(acc, x) return acc + x * x end
local xs = {}
for i = 0, 19999 do xs[#xs + 1] = i % 1000 end
local total = 0
for r = 1, 20 do
  local acc = 0
  for i = 1, #xs do acc = step(acc, xs[i]) end
  total = total + acc
end
print(total)
