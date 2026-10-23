-- 十六进制格式化与解析（标准等价：string.format与tonumber）
local total = 0
for i = 0, 399999 do
  local s = string.format("%x", i * 37)
  total = total + tonumber(s, 16) + #s
end
print(total)
