-- (同11的参照) 去重保留首次出现顺序
local xs, ss = {}, {}
for i = 0, 19999 do xs[#xs + 1] = (i * 7919) % 10000 end
for i = 0, 4999 do ss[#ss + 1] = "k" .. tostring((i * 31) % 2500) end
local total = 0
for r = 0, 9 do
  local seen, u = {}, {}
  for i = 1, #xs do local x = xs[i]; if not seen[x] then seen[x] = true; u[#u + 1] = x end end
  total = total + #u + u[1] + u[#u]
  local seen_s, us = {}, {}
  for i = 1, #ss do local s = ss[i]; if not seen_s[s] then seen_s[s] = true; us[#us + 1] = s end end
  total = total + #us
end
print(total)
