-- 反三角、立方根与浮点余数
local total = 0.0
for i = 0, 99999 do
  local x = (i % 2000) / 1000.0 - 1.0
  local y = x * 8.0
  local c = (y < 0 and -1 or 1) * math.abs(y) ^ (1.0 / 3.0)
  total = total + math.asin(x) + math.acos(x) + math.atan(x, 0.5) + c + math.fmod(i * 1.0, 7.5)
end
print(math.floor(math.sqrt(math.floor(total * 1000.0))))
