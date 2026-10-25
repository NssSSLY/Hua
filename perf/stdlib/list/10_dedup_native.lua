-- 去相邻重复
local xs, ss = {}, {}
for i = 0, 19999 do xs[#xs + 1] = (i // 2 * 7) % 1000 end
for i = 0, 19999 do ss[#ss + 1] = "k" .. tostring((i // 2) % 2500) end
local total = 0
for r = 0, 9 do
  local out, prev = {}, nil
  for i = 1, #xs do local x = xs[i]; if x ~= prev then out[#out + 1] = x; prev = x end end
  total = total + #xs - #out + #out
  local outs, ps = {}, nil
  for i = 1, #ss do local s = ss[i]; if s ~= ps then outs[#outs + 1] = s; ps = s end end
  total = total + #ss - #outs + #outs * 2
end
print(total)
