-- 手写repeat与左侧补齐（同一算法，不用string.rep与format）
local function repeat_text(text, n)
  local out = ""
  for i = 1, n do out = out .. text end
  return out
end
local function pad_left(text, width, fill)
  local out = text
  while #out < width do out = fill .. out end
  return out
end
local total = 0
for i = 0, 39999 do
  total = total + #repeat_text("ab", 16)
  total = total + #pad_left(tostring(i), 12, "0")
end
print(total)
