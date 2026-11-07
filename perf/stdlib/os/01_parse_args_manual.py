# 参数解析（手写循环，不用argparse的语义差异）
argv = ["input.txt", "--verbose", "--name=hua", "-n", "second", "--name", "third", "output.txt", "--", "--literal"]
total = 0
for r in range(4000):
    positional = names = 0
    verbose = rest = False
    i = 0
    while i < len(argv):
        t = argv[i]
        if rest:
            positional += 1
        elif t == "--":
            rest = True
        elif t == "--verbose":
            verbose = True
        elif t.startswith("--name="):
            names += 1
        elif t in ("--name", "-n"):
            names += 1
            i += 1
        else:
            positional += 1
        i += 1
    total += positional + names + (1 if verbose else 0)
print(total)
