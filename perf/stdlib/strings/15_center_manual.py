# 居中补齐（差额奇数时多出的放右侧，与strings.center相同；str.center奇偶规则不同）
words = ["a", "héllo", "中文字", "😀😀", "plain ascii text", "aé中😀"]
total = 0
for r in range(20000):
    for w in words:
        missing = 21 - len(w)
        left = missing // 2
        total += len(("*" * left + w + "*" * (missing - left)).encode())
print(total)
