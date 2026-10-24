-- 生成UUID v4（非安全随机，仅作速度参照）
local ok = 0
local hex = "0123456789abcdef"
for i = 1, 100000 do
  local t = {}
  for j = 1, 36 do
    if j == 9 or j == 14 or j == 19 or j == 24 then t[j] = "-"
    elseif j == 15 then t[j] = "4"
    elseif j == 20 then local v = math.random(8, 11) + 1; t[j] = hex:sub(v, v)
    else local v = math.random(1, 16); t[j] = hex:sub(v, v) end
  end
  local u = table.concat(t)
  if #u == 36 and u:sub(15, 15) == "4" and ("89ab"):find(u:sub(20, 20), 1, true) then ok = ok + 1 end
end
print(ok)
