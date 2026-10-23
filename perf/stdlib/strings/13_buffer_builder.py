# 字符串构建器（标准等价：io.StringIO）
import io
total = 0
for round in range(10):
    b = io.StringIO()
    for i in range(20000):
        b.write("ab")
    s = b.getvalue()
    total += len(s)
print(total)
