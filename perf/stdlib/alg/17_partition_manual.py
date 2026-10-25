# 稳定二分
def keep(x):
    return x % 3 == 0
xs = [(i * 7919) % 10007 for i in range(20000)]
total = 0
for r in range(10):
    ys = [x for x in xs if keep(x)]
    ns = [x for x in xs if not keep(x)]
    total += len(ys) + ys[0] + ns[-1]
print(total)
