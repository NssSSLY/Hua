-- 单词计数
local words = {}
for i = 0, 19999 do words[#words + 1] = "w" .. ((i * 7) % 500) end
local total = 0
for r = 1, 20 do
  local counts, n = {}, 0
  for _, w in ipairs(words) do local c = counts[w]; if c == nil then n = n + 1; c = 0 end; counts[w] = c + 1 end
  total = total + counts["w0"] + counts["w499"] + n
end
print(total)
