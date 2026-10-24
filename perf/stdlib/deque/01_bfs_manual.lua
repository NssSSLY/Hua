-- 网格BFS（表加头尾下标）
local W = 300
local total = 0
for r = 1, 3 do
  local dist = {}
  for i = 0, W * W - 1 do dist[i] = -1 end
  dist[0] = 0
  local q, head, tail = {[1] = 0}, 1, 1
  while head <= tail do
    local c = q[head]; head = head + 1
    local x, y = c % W, c // W
    local nb = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}}
    for k = 1, 4 do
      local nx, ny = nb[k][1], nb[k][2]
      if nx >= 0 and nx < W and ny >= 0 and ny < W then
        local n = ny * W + nx
        if dist[n] < 0 and (nx * 7 + ny * 13) % 11 ~= 0 then dist[n] = dist[c] + 1; tail = tail + 1; q[tail] = n end
      end
    end
  end
  for i = 0, W * W - 1 do if dist[i] > 0 then total = total + dist[i] end end
end
print(total)
