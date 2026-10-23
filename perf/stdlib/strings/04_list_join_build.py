# 先收集再join
total = 0
for round in range(10):
    parts = []
    for i in range(20000):
        parts.append("ab")
    s = "".join(parts)
    total += len(s)
print(total)
