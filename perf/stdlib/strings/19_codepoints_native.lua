-- 码点往返
local text = string.rep("aé中😀", 2000)
local total = 0
for r = 1, 10 do
  local cs = {}
  for _, c in utf8.codes(text) do cs[#cs + 1] = c end
  local parts = {}
  for i = 1, #cs do parts[i] = utf8.char(cs[i]) end
  local back = table.concat(parts)
  total = total + #cs + cs[#cs] + #back
end
print(total)
