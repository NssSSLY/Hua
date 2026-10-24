-- (同13的参照) 连乘
local xs, fs = {}, {}
for i = 0, 9999 do
  xs[#xs + 1] = (i % 1000 == 999) and 3 or 1
  fs[#fs + 1] = (i % 2 == 0) and 1.0001 or 0.9999
end
local total, near = 0, 0
for r = 0, 99 do
  local p = 1
  for i = 1, #xs do p = p * xs[i] end
  total = total + p
  local q = 1.0
  for i = 1, #fs do q = q * fs[i] end
  if q > 0.99 and q < 1.01 then near = near + 1 end
end
print(total .. " " .. near)
