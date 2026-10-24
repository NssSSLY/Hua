# 就地保留满足条件的项
check = 0
for r in range(20):
    l = [(i * 13) % 1000 for i in range(20000)]
    before = len(l)
    l[:] = [x for x in l if x % 4 != 0]
    check += before - len(l) + l[0] + l[-1]
print(check)
