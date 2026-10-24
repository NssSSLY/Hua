-- 按固定块长切分
local xs = {}
for i = 0, 19999 do xs[#xs + 1] = i end
local total = 0
for r = 0, 9 do
  local c = {}
  for i = 1, #xs, 16 do
    local b = {}
    for j = i, math.min(i + 15, #xs) do b[#b + 1] = xs[j] end
    c[#c + 1] = b
  end
  total = total + #c + c[#c][1] + #c[#c]
end
print(total)
