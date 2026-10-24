# 去重保留首次出现顺序
xs = [(i * 7919) % 10000 for i in range(20000)]
ss = ["k" + str((i * 31) % 2500) for i in range(5000)]
total = 0
for r in range(10):
    u = list(dict.fromkeys(xs))
    total += len(u) + u[0] + u[-1]
    total += len(list(dict.fromkeys(ss)))
print(total)
