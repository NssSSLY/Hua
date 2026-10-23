# 字符串键热查找
m = {}
keys = []
for i in range(2000):
    k = "key" + str(i)
    m[k] = i
    keys.append(k)
total = 0
misses = 0
for r in range(250):
    for k in keys:
        if k in m:
            total += m[k]
    if ("missing" + str(r)) not in m:
        misses += 1
print(total, misses)
