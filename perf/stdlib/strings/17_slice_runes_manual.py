# 码点切片
text = "aé中😀" * 100
total = 0
for r in range(4):
    for k in range(200):
        start = (k * 7) % 340
        total += len(text[start:start + 50].encode())
print(total)
