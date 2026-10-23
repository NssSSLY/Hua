-- 字符串构建器（标准等价：table.concat）
local total = 0
for round = 0, 9 do
  local parts = {}
  for i = 1, 20000 do parts[i] = "ab" end
  local s = table.concat(parts)
  total = total + #s
end
print(total)
