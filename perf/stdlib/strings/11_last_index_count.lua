-- 末次出现位置与出现次数（Lua无rfind/count，用plain find循环）
local text = "the quick brown fox jumps over the lazy dog and the cat sleeps by the door"
local function count(s, sub)
  local n, at = 0, 1
  while true do
    local a, b = string.find(s, sub, at, true)
    if not a then return n end
    n = n + 1; at = b + 1
  end
end
local function last_index(s, sub)
  local last, at = -1, 1
  while true do
    local a = string.find(s, sub, at, true)
    if not a then return last end
    last = a - 1; at = a + 1
  end
end
local total = 0
for i = 0, 149999 do
  total = total + last_index(text, "the") + count(text, "the") + count(text, "o")
end
print(total)
