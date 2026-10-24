-- 滑动窗口最大值（表加头尾下标）
local xs, x = {}, 1
for i = 0, 99999 do x = (x * 1103515245 + 12345) % 2147483648; xs[i] = x % 100000 end
local total = 0
for r = 1, 3 do
  local q, head, tail = {}, 1, 0
  for i = 0, 99999 do
    local v = xs[i]
    while tail >= head and xs[q[tail]] <= v do tail = tail - 1 end
    tail = tail + 1; q[tail] = i
    if q[head] <= i - 50 then head = head + 1 end
    if i >= 49 then total = total + xs[q[head]] end
  end
end
print(total)
