-- 按结构字段排序（table.sort不稳定，用(score,id)作比较保证与稳定排序同序）
local items = {}
for i = 0, 4999 do items[#items + 1] = {id = i, score = (i * 7919) % 1000} end
local check = 0
for r = 1, 10 do
  local ordered = {}
  for i = 1, #items do ordered[i] = items[i] end
  table.sort(ordered, function(a, b) if a.score ~= b.score then return a.score < b.score end return a.id < b.id end)
  for j = 0, 4999, 97 do check = check + j * ordered[j + 1].id end
end
print(check)
