-- 滑动窗口
local xs = {}
for i = 0, 4999 do xs[#xs + 1] = (i * 7919) % 10007 end
local total = 0
for r = 0, 9 do
  local ws = {}
  for i = 1, #xs - 7 do
    local w = {}
    for j = i, i + 7 do w[#w + 1] = xs[j] end
    ws[#ws + 1] = w
  end
  total = total + #ws
  for k = 1, #ws do local w = ws[k]; total = total + w[1] + w[8] end
end
print(total)
