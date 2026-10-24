-- 洗牌与抽样（输出与排列无关的和）
math.randomseed(7)
local l = {}
for i = 0, 9999 do l[#l + 1] = i end
local total = 0
for k = 1, 20 do
  for i = #l, 2, -1 do local j = math.random(1, i); l[i], l[j] = l[j], l[i] end
  local s = 0
  for i = 1, #l do s = s + l[i] end
  local idx = {}
  for i = 1, #l do idx[i] = i end
  for i = 1, 100 do local j = math.random(i, #idx); idx[i], idx[j] = idx[j], idx[i] end
  total = total + s + 100
end
print(total)
