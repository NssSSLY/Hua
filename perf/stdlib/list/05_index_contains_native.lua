-- 线性查找下标与存在
local l = {}
for i = 0, 1999 do l[#l + 1] = (i * 7) % 3001 end
local s, found = 0, 0
for q = 0, 399 do
  local t, at = (q * 13) % 3001, -1
  for i = 1, #l do if l[i] == t then at = i - 1; break end end
  s = s + at
  for i = 1, #l do if l[i] == t then found = found + 1; break end end
end
print(s .. " " .. found)
