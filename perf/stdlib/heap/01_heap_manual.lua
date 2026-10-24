local function hpush(h, x)
  local i = #h + 1; h[i] = x
  while i > 1 do local p = i // 2; if h[p] <= x then break end; h[i] = h[p]; i = p end
  h[i] = x
end
local function hpop(h)
  local top, last = h[1], h[#h]; h[#h] = nil
  local n = #h
  if n > 0 then
    local i = 1
    while 2 * i <= n do
      local c = 2 * i
      if c + 1 <= n and h[c + 1] < h[c] then c = c + 1 end
      if h[c] >= last then break end
      h[i] = h[c]; i = c
    end
    h[i] = last
  end
  return top
end
-- 堆：入堆5万个再全部取出
local total = 0
for r = 0, 2 do
  local h, x = {}, r + 1
  for i = 1, 50000 do x = (x * 1103515245 + 12345) % 2147483648; hpush(h, x % 1000000) end
  local k = 0
  while #h > 0 do total = total + k * hpop(h) % 1000003; k = k + 1 end
end
print(total)
