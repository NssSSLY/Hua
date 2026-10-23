# 合并两个map
added = 0
size = 0
for r in range(20):
    dst = {i * 2: i for i in range(3000)}
    src = {i * 3: i for i in range(3000)}
    before = len(dst)
    dst.update(src)
    added += len(dst) - before
    size += len(dst)
print(added, size)
