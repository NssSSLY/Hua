-- base64编码/解码往返（手写）
local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local rev = {}
for i = 1, 64 do rev[alphabet:byte(i)] = i - 1 end
local t = {}
for i = 0, 4095 do t[#t + 1] = string.char((i * 7) % 256) end
local data = table.concat(t)
local crc_table = {}
for i = 0, 255 do local c = i; for _ = 1, 8 do if c & 1 == 1 then c = 0xedb88320 ~ (c >> 1) else c = c >> 1 end end; crc_table[i] = c end
local function crc32(s) local c = 0xffffffff; for i = 1, #s do c = crc_table[(c ~ s:byte(i)) & 255] ~ (c >> 8) end; return c ~ 0xffffffff end
local function enc(s)
  local out = {}
  for i = 1, #s, 3 do
    local a, b, c = s:byte(i, i + 2)
    local n = (a << 16) | ((b or 0) << 8) | (c or 0)
    out[#out + 1] = alphabet:sub((n >> 18) + 1, (n >> 18) + 1) .. alphabet:sub(((n >> 12) & 63) + 1, ((n >> 12) & 63) + 1)
      .. (b and alphabet:sub(((n >> 6) & 63) + 1, ((n >> 6) & 63) + 1) or "=") .. (c and alphabet:sub((n & 63) + 1, (n & 63) + 1) or "=")
  end
  return table.concat(out)
end
local function dec(s)
  local out = {}
  for i = 1, #s, 4 do
    local a, b, c, d = s:byte(i, i + 3)
    local n = (rev[a] << 18) | (rev[b] << 12) | ((rev[c] or 0) << 6) | (rev[d] or 0)
    out[#out + 1] = string.char(n >> 16)
    if c ~= 61 then out[#out + 1] = string.char((n >> 8) & 255) end
    if d ~= 61 then out[#out + 1] = string.char(n & 255) end
  end
  return table.concat(out)
end
local total = 0
for r = 1, 30 do
  local text = enc(data)
  total = total + #text + crc32(dec(text))
end
print(total)
