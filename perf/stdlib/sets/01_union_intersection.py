# 集合并/交/差/子集
u = n = d = subset = 0
for r in range(20):
    a = set(range(5000))
    b = set(i + 2500 for i in range(5000))
    un = a | b
    inter = a & b
    diff = a - b
    u += len(un)
    n += len(inter)
    d += len(diff)
    if inter <= a:
        subset += 1
print(u, n, d, subset)
