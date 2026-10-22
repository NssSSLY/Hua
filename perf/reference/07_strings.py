# 07_strings：路径字符串拼接、split
def depth(path):
    return len(path.split("#@")) - 1
def main():
    total = 0
    for i in range(0, 150000):
        origin = "module" + str(i % 13) + "|#@a#@b" + str(i % 5)
        parts = origin.split("|")
        total += depth(parts[1]) + len(parts[0])
    print(total)
main()
