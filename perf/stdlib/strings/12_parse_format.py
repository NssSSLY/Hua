# 十六进制格式化与解析（标准等价：format与int(s,16)）
total = 0
for i in range(400000):
    s = format(i * 37, "x")
    total += int(s, 16) + len(s)
print(total)
