-- 05_map_ops：table 字符串键插入、查询、遍历
local m = {}
for i = 0, 19999 do m["k" .. tostring(i)] = (i % 2 == 0) end
local hits = 0
for round = 0, 19 do
  for i = 0, 19999 do
    if m["k" .. tostring(i * 7 % 20000)] ~= nil and m["k" .. tostring(i)] then hits = hits + 1 end
  end
end
local count = 0
for key, flag in pairs(m) do if flag then count = count + 1 end end
print(hits .. " " .. count)
