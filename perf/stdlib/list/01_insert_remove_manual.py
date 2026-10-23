# 中间插入与删除
total = 0
for r in range(5):
    l = list(range(1000))
    for j in range(200):
        l.insert((j * 37) % len(l), -j)
    for j in range(200):
        total += l.pop((j * 53) % len(l))
    total += len(l) + l[0] + l[-1]
print(total)
