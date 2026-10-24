# 随机整数与浮点（只输出与序列无关的计数）
import random
r = random.Random(42)
n = 0
for i in range(300000):
    if 0 <= r.randrange(0, 1000) < 1000:
        n += 1
    if 0.0 <= r.random() < 1.0:
        n += 1
print(n)
