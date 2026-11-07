-- JSON构造（Lua没有内建JSON，手写拼文本）
local total = 0
for r = 1, 20 do
  local parts = {}
  for i = 0, 999 do
    parts[#parts + 1] = '{"id":' .. i .. ',"name":"n' .. i .. '","ok":true}'
  end
  total = total + #("[" .. table.concat(parts, ",") .. "]")
end
print(total)
