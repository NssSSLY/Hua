# (同13的参照) 连乘
import math
xs = [3 if i % 1000 == 999 else 1 for i in range(10000)]
fs = [1.0001 if i % 2 == 0 else 0.9999 for i in range(10000)]
total = 0
near = 0
for r in range(100):
    total += math.prod(xs)
    q = math.prod(fs)
    if 0.99 < q < 1.01:
        near += 1
print(total, near)
