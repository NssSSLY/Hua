# 按行与按空白切分（标准等价：splitlines与split()）
text = "alpha  beta\tgamma\ndelta   epsilon\n\nzeta eta\ttheta\n"
total = 0
for i in range(20000):
    for line in text.splitlines():
        if len(line) == 0:
            continue
        total += 1
        for word in line.split():
            total += len(word)
print(total)
