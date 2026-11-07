-- 居中补齐
local words = {"a", "héllo", "中文字", "😀😀", "plain ascii text", "aé中😀"}
local total = 0
for r = 1, 20000 do
  for i = 1, #words do
    local w = words[i]
    local missing = 21 - utf8.len(w)
    local left = missing // 2
    total = total + #(string.rep("*", left) .. w .. string.rep("*", missing - left))
  end
end
print(total)
