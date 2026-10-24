-- 过滤、存在与查找
local function keep(x) return x % 3 == 0 end
local xs = {}
for i = 0, 19999 do xs[#xs + 1] = (i * 7) % 20000 end
local total = 0
for r = 0, 19 do
  local kept = {}
  for i = 1, #xs do if keep(xs[i]) then kept[#kept + 1] = xs[i] end end
  total = total + #kept
  for i = 1, #xs do if xs[i] > 19990 then total = total + 1; break end end
  local at = -1
  for i = 1, #xs do if xs[i] == 19999 - r then at = i - 1; break end end
  total = total + at
end
print(total)
