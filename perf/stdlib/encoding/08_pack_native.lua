-- 端序打包
local total = 0
for i = 0, 29999 do
  local v = i * 7919
  total = total + string.unpack(">I4", string.pack(">I4", v))
  local n = (i % 65536) - 32768
  total = total + string.unpack("<i2", string.pack("<i2", n))
end
print(total)
