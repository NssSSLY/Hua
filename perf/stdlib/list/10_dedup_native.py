# 去相邻重复
from itertools import groupby
xs = [(i // 2 * 7) % 1000 for i in range(20000)]
ss = ["k" + str((i // 2) % 2500) for i in range(20000)]
total = 0
for r in range(10):
    out = [k for k, _ in groupby(xs)]
    total += len(xs) - len(out) + len(out)
    out_s = [k for k, _ in groupby(ss)]
    total += len(ss) - len(out_s) + len(out_s) * 2
print(total)
