-- 千分位
local total = 0
for i = 0, 29999 do
  local s = tostring(i * 7919):reverse():gsub("(%d%d%d)", "%1,"):reverse()
  if s:sub(1, 1) == "," then s = s:sub(2) end
  total = total + #s
end
print(total)
