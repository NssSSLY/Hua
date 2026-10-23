-- repeat与左侧补齐（标准等价：string.rep）
local total = 0
for i = 0, 39999 do
  total = total + #string.rep("ab", 16)
  local s = tostring(i)
  if #s < 12 then s = string.rep("0", 12 - #s) .. s end
  total = total + #s
end
print(total)
