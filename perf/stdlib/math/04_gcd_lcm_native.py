# 最大公约数与最小公倍数
import math
total = 0
for i in range(200000):
    a = i % 1000 + 1
    b = (i * 7) % 1000 + 1
    total += math.gcd(a, b) + math.lcm(a, b)
print(total)
