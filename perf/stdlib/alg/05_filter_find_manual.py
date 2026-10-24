# 过滤、存在与查找
def keep(x):
    return x % 3 == 0
xs = [(i * 7) % 20000 for i in range(20000)]
total = 0
for r in range(20):
    kept = [x for x in xs if keep(x)]
    total += len(kept)
    total += 1 if any(x > 19990 for x in xs) else 0
    total += next((i for i, x in enumerate(xs) if x == 19999 - r), -1)
print(total)
