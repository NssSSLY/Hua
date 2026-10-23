# 整数与字符串互转
total = 0
for i in range(800000):
    s = str(i * 37)
    total += int(s) + len(s)
print(total)
