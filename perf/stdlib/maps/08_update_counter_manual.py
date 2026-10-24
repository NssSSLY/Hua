# 单词计数
words = ["w" + str((i * 7) % 500) for i in range(20000)]
total = 0
for r in range(20):
    counts = {}
    for w in words:
        counts[w] = counts.get(w, 0) + 1
    total += counts["w0"] + counts["w499"] + len(counts)
print(total)
