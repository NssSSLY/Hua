-- 就地排序与反转（Lua无内建reverse，手写交换）
local check = 0
for r = 0, 9 do
  local x, l = r + 1, {}
  for i = 1, 20000 do x = (x * 1103515245 + 12345) % 2147483648; l[i] = x % 100000 end
  table.sort(l)
  check = check + l[1] + l[10001] + l[20000]
  local i, j = 1, #l
  while i < j do l[i], l[j] = l[j], l[i]; i = i + 1; j = j - 1 end
  check = check + l[1] - l[20000]
end
print(check)
