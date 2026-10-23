# 手写按空白切分与按行切分（同一组合，不用split()无参数版本）
text = "alpha  beta\tgamma\ndelta   epsilon\n\nzeta eta\ttheta\n"
total = 0
for i in range(20000):
    for line in text.split("\n"):
        if len(line) == 0:
            continue
        total += 1
        for word in line.replace("\t", " ").split(" "):
            if len(word) > 0:
                total += len(word)
print(total)
