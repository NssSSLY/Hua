# 按行读取
import os, tempfile
fd, path = tempfile.mkstemp(suffix=".txt")
with os.fdopen(fd, "w", newline="\n") as out:
    out.write("the quick brown fox jumps over\n" * 20000)
total = 0
for r in range(3):
    with open(path, newline="\n") as h:
        for line in h:
            total += len(line)
os.remove(path)
print(total)
