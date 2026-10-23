# 去重计数
xs = [(i * 7919) % 3000 for i in range(20000)]
total = 0
for r in range(20):
    total += len(set(xs))
print(total)
