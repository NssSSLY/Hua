-- 码点切片
local text = string.rep("aé中😀", 100)
local total = 0
for r = 1, 4 do
  for k = 0, 199 do
    local start = (k * 7) % 340
    total = total + #string.sub(text, utf8.offset(text, start + 1), utf8.offset(text, start + 51) - 1)
  end
end
print(total)
