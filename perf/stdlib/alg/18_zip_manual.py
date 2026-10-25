# 两两组合
def mix(x, y):
    return x * 3 + y
xs = [(i * 7919) % 10007 for i in range(20000)]
ys = [(i * 31) % 997 for i in range(19000)]
total = 0
for r in range(10):
    z = [mix(x, y) for x, y in zip(xs, ys)]
    total += len(z) + z[0] + z[-1]
print(total)
