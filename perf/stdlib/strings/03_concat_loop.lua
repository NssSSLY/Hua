-- 逐次拼接构造字符串
local total = 0
for round = 0, 4 do
  local s = ""
  for i = 0, 19999 do s = s .. "ab" end
  total = total + #s
end
print(total)
