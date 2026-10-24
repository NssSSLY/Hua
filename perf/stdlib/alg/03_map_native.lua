-- 逐项变换
local function scale(x) return x * 3 + 1 end
local xs = {}
for i = 0, 19999 do xs[#xs + 1] = i % 1000 end
local total = 0
for r = 1, 20 do
  local out = {}
  for i = 1, #xs do out[i] = scale(xs[i]) end
  for i = 1, #out do total = total + out[i] end
end
print(total)
