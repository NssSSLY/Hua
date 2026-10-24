# 反三角、立方根与浮点余数
import math
total = 0.0
for i in range(100000):
    x = (i % 2000) / 1000.0 - 1.0
    c = math.copysign(abs(x * 8.0) ** (1.0 / 3.0), x)
    total += math.asin(x) + math.acos(x) + math.atan2(x, 0.5) + c + math.fmod(float(i), 7.5)
print(math.isqrt(int(total * 1000.0)))
