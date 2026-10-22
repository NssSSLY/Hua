# 06_map_merge：逐项写法的集合相等与合并（与 Hua 版本相同算法，不用 dict ==）
def same_flags(a, b):
    if len(a) != len(b):
        return False
    for name, flag in a.items():
        if name not in b or flag != b[name]:
            return False
    return True
def merged(a, b):
    out = dict(a)
    for origin, readonly in b.items():
        out[origin] = readonly or out.get(origin, False)
    return out
def main():
    a, b = {}, {}
    for i in range(0, 400):
        a["m|#@f" + str(i)] = False
        b["m|#@f" + str(i)] = i % 3 == 0
    same = 0
    size = 0
    for round in range(0, 300):
        if same_flags(a, a):
            same += 1
        if same_flags(a, b):
            same += 1
        c = merged(a, b)
        size += len(c)
    print(same, size)
main()
