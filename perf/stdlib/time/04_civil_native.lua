-- 由Unix毫秒拆出UTC分量并格式化为RFC 3339
local total, length = 0, 0
for i = 0, 99999 do
  local ms = (i * 86399937 * 13) % 4102444800000
  local t = os.date("!*t", ms // 1000)
  local frac = ms % 1000
  total = total + t.year + t.month + t.day + t.hour + t.min + t.sec + frac
  local text = os.date("!%Y-%m-%dT%H:%M:%S", ms // 1000) .. (frac ~= 0 and string.format(".%03d", frac) or "") .. "Z"
  length = length + #text
end
print(total .. " " .. length)
