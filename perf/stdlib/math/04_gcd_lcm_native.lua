-- 最大公约数与最小公倍数（Lua无内建，手写）
local function gcd(a, b) while b ~= 0 do a, b = b, a % b end return a end
local total = 0
for i = 0, 199999 do
  local a, b = i % 1000 + 1, (i * 7) % 1000 + 1
  local g = gcd(a, b)
  total = total + g + a // g * b
end
print(total)
