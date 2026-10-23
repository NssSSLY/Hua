-- 去重计数
local xs = {}
for i = 0, 19999 do xs[#xs + 1] = (i * 7919) % 3000 end
local total = 0
for r = 1, 20 do
  local seen, c = {}, 0
  for _, x in ipairs(xs) do if not seen[x] then seen[x] = true; c = c + 1 end end
  total = total + c
end
print(total)
