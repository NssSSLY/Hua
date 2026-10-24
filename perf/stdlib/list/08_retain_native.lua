-- 就地保留满足条件的项
local check = 0
for r = 1, 20 do
  local l = {}
  for i = 0, 19999 do l[#l + 1] = (i * 13) % 1000 end
  local before, n = #l, 0
  for i = 1, before do if l[i] % 4 ~= 0 then n = n + 1; l[n] = l[i] end end
  for i = before, n + 1, -1 do l[i] = nil end
  check = check + before - #l + l[1] + l[#l]
end
print(check)
