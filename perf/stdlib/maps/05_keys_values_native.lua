-- 按键有序取键值
local m = {}
for i = 0, 4999 do m[(i * 7919) % 5000] = i end
local total = 0
for r = 1, 20 do
  local keys = {}
  for k in pairs(m) do keys[#keys + 1] = k end
  table.sort(keys)
  for j = 1, #keys do total = total + (j - 1) * m[keys[j]] end
end
print(total)
