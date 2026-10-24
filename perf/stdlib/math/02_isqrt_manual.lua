-- 整数平方根（浮点初值加整数校正）
local total = 0
for i = 0, 199999 do
  local n = i * 999983 + (i * i) % 1000003
  local x = math.floor(math.sqrt(n))
  while x * x > n do x = x - 1 end
  while (x + 1) * (x + 1) <= n do x = x + 1 end
  total = total + x
end
print(total)
