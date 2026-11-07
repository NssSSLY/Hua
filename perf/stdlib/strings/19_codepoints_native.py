# 码点往返
text = "aé中😀" * 2000
total = 0
for r in range(10):
    cs = list(map(ord, text))
    back = "".join(map(chr, cs))
    total += len(cs) + cs[-1] + len(back.encode())
print(total)
