# 整数平方根
import math
total = 0
for i in range(200000):
    n = i * 999983 + (i * i) % 1000003
    total += math.isqrt(n)
print(total)
