# 带缺省值读取与删除取值
total = 0
for r in range(40):
    m = {i: i * 3 for i in range(3000)}
    for i in range(6000):
        total += m.get(i, 1)
    for i in range(0, 3000, 2):
        total += m.pop(i)
    total += len(m)
print(total)
