# 逐次拼接构造字符串
total = 0
for round in range(5):
    s = ""
    for i in range(20000):
        s = s + "ab"
    total += len(s)
print(total)
