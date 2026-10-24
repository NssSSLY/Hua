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
-- 网格Dijkstra（键编码）
local W = 150
local total = 0
for r = 1, 2 do
  local dist = {}
  for i = 0, W * W - 1 do dist[i] = 1000000000000000000 end
  dist[0] = 0
  local h = {0}
  while #h > 0 do
    local e = hpop(h)
    local d, c = e // 100000, e % 100000
    if d <= dist[c] then
      local x, y = c % W, c // W
      local nb = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}}
      for k = 1, 4 do
        local nx, ny = nb[k][1], nb[k][2]
        if nx >= 0 and nx < W and ny >= 0 and ny < W then
          local n = ny * W + nx
          local nd = d + (nx * 31 + ny * 17) % 9 + 1
          if nd < dist[n] then dist[n] = nd; hpush(h, nd * 100000 + n) end
        end
      end
    end
  end
  for i = 0, W * W - 1 do total = total + dist[i] end
end
print(total)
