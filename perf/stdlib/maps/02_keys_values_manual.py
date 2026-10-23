# 按键有序取键值
m = {}
for i in range(5000):
    m[(i * 7919) % 5000] = i
total = 0
for r in range(20):
    for j, (k, v) in enumerate(sorted(m.items())):
        total += j * v
print(total)
