-- 中间插入与删除
local total = 0
for r = 1, 5 do
  local l = {}
  for i = 0, 999 do l[#l + 1] = i end
  for j = 0, 199 do table.insert(l, (j * 37) % #l + 1, -j) end
  for j = 0, 199 do total = total + table.remove(l, (j * 53) % #l + 1) end
  total = total + #l + l[1] + l[#l]
end
print(total)
