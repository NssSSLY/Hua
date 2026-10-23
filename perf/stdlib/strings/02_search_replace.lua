-- 查找与替换（plain find，非模式匹配）
local text = "the quick brown fox jumps over the lazy dog and the cat sleeps"
local total = 0
for i = 0, 149999 do
  if string.find(text, "lazy", 1, true) then total = total + 1 end
  total = total + (string.find(text, "cat", 1, true) - 1)
  local changed = string.gsub(text, "the", "THE")
  total = total + #changed
end
print(total)
