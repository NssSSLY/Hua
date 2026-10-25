# 滑动窗口
xs = [(i * 7919) % 10007 for i in range(5000)]
total = 0
for r in range(10):
    ws = [xs[i:i + 8] for i in range(len(xs) - 7)]
    total += len(ws)
    for w in ws:
        total += w[0] + w[7]
print(total)
