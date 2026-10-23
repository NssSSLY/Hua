# 线性查找下标与存在
l = [(i * 7) % 3001 for i in range(2000)]
s = 0
found = 0
for q in range(400):
    t = (q * 13) % 3001
    s += l.index(t) if t in l else -1
    if t in l:
        found += 1
print(s, found)
