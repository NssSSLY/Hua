-- 整数与字符串互转
local total = 0
for i = 0, 799999 do
  local s = tostring(i * 37)
  total = total + math.tointeger(tonumber(s)) + #s
end
print(total)
