-- 07_strings：路径字符串拼接、split
local function split(s, sep)
  local out, start = {}, 1
  while true do
    local i, j = string.find(s, sep, start, true)
    if not i then out[#out + 1] = string.sub(s, start); return out end
    out[#out + 1] = string.sub(s, start, i - 1); start = j + 1
  end
end
local function depth(path) return #split(path, "#@") - 1 end
local total = 0
for i = 0, 149999 do
  local origin = "module" .. tostring(i % 13) .. "|#@a#@b" .. tostring(i % 5)
  local parts = split(origin, "|")
  total = total + depth(parts[2]) + #parts[1]
end
print(total)
