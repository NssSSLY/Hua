-- 稳定二分
local function keep(x) return x % 3 == 0 end
local xs = {}
for i = 0, 19999 do xs[#xs + 1] = (i * 7919) % 10007 end
local total = 0
for r = 0, 9 do
  local ys, ns = {}, {}
  for i = 1, #xs do local x = xs[i]; if keep(x) then ys[#ys + 1] = x else ns[#ns + 1] = x end end
  total = total + #ys + ys[1] + ns[#ns]
end
print(total)
