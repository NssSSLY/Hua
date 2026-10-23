# repeat与左侧补齐（标准等价：*与rjust）
total = 0
for i in range(40000):
    total += len("ab" * 16)
    total += len(str(i).rjust(12, "0"))
print(total)
