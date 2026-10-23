-- 带缺省值读取与删除取值
local total = 0
for r = 1, 40 do
  local m, n = {}, 0
  for i = 0, 2999 do m[i] = i * 3; n = n + 1 end
  for i = 0, 5999 do local v = m[i]; if v ~= nil then total = total + v else total = total + 1 end end
  for i = 0, 2999, 2 do local v = m[i]; if v ~= nil then total = total + v; m[i] = nil; n = n - 1 end end
  total = total + n
end
print(total)
