-- 随机整数与浮点（只输出与序列无关的计数）
math.randomseed(42)
local n = 0
for i = 1, 300000 do
  local v = math.random(0, 999)
  if v >= 0 and v < 1000 then n = n + 1 end
  local f = math.random()
  if f >= 0.0 and f < 1.0 then n = n + 1 end
end
print(n)
