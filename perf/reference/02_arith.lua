-- 02_arith：范围 for + 整数运算
local total = 0
for i = 0, 1999999 do total = (total + i * 3) % 1000003 end
print(total)
