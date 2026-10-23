-- 去空白与大小写
local raw = "   Mixed Case Text With Spaces And Tabs\t "
local total = 0
for i = 0, 99999 do
  local t = string.match(raw, "^%s*(.-)%s*$")
  total = total + #string.lower(t) + #string.upper(t)
end
print(total)
