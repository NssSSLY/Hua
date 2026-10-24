-- 十六进制编码/解码往返（CRC32手写）
local t = {}
for i = 0, 4095 do t[#t + 1] = string.char(i % 256) end
local data = table.concat(t)
local crc_table = {}
for i = 0, 255 do local c = i; for _ = 1, 8 do if c & 1 == 1 then c = 0xedb88320 ~ (c >> 1) else c = c >> 1 end end; crc_table[i] = c end
local function crc32(s) local c = 0xffffffff; for i = 1, #s do c = crc_table[(c ~ s:byte(i)) & 255] ~ (c >> 8) end; return c ~ 0xffffffff end
local total = 0
for r = 1, 50 do
  local parts = {}
  for i = 1, #data do parts[i] = string.format("%02x", data:byte(i)) end
  local text = table.concat(parts)
  local out = {}
  for i = 1, #text, 2 do out[#out + 1] = string.char(tonumber(text:sub(i, i + 1), 16)) end
  total = total + #text + crc32(table.concat(out))
end
print(total)
