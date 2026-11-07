# UTF-8校验与逐字符解码
data = ("aé中😀" * 2000).encode()
total = 0
for r in range(5):
    text = data.decode("utf-8")
    total += 1
    for c in text:
        total += ord(c)
print(total)
