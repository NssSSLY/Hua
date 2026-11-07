# 千分位
total = 0
for i in range(30000):
    total += len(f"{i * 7919:,}")
print(total)
