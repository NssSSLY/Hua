-- 限次切分与限次替换（标准等价：find与gsub的次数参数）
local line = "key=value=with=equals and the the the end"
local total = 0
for i = 0, 149999 do
  local at = string.find(line, "=", 1, true)
  local rest = string.sub(line, at + 1)
  total = total + 2 + #rest
  local changed = string.gsub(line, "the", "THE", 2)
  total = total + #changed
end
print(total)
