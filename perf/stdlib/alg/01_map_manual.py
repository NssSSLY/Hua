# 逐项变换
def scale(x):
    return x * 3 + 1
xs = [i % 1000 for i in range(20000)]
total = 0
for r in range(20):
    total += sum(map(scale, xs))
print(total)
