-- UTF-8校验与逐字符解码
local data = string.rep("aé中😀", 2000)
local total = 0
for r = 1, 5 do
  if utf8.len(data) then total = total + 1 end
  for _, c in utf8.codes(data) do total = total + c end
end
print(total)
