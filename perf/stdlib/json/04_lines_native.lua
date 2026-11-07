-- JSONL（Lua没有内建JSON，用模式取字段）
local text = string.rep('{"id":1,"name":"abc"}\n', 2000)
local total = 0
for r = 1, 10 do
  for line in text:gmatch("[^\n]+") do
    total = total + tonumber(line:match('"id":(%d+)'))
  end
end
print(total)
