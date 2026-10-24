# 归约
from functools import reduce
def step(acc, x):
    return acc + x * x
xs = [i % 1000 for i in range(20000)]
total = 0
for r in range(20):
    total += reduce(step, xs, 0)
print(total)
