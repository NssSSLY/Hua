# (同12的参照) 按固定块长切分
xs = list(range(20000))
total = 0
for r in range(10):
    c = [xs[i:i + 16] for i in range(0, len(xs), 16)]
    total += len(c) + c[-1][0] + len(c[-1])
print(total)
