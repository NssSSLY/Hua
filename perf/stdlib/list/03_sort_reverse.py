# 就地排序与反转
check = 0
for r in range(10):
    x = r + 1
    l = []
    for i in range(20000):
        x = (x * 1103515245 + 12345) % 2147483648
        l.append(x % 100000)
    l.sort()
    check += l[0] + l[10000] + l[19999]
    l.reverse()
    check += l[0] - l[19999]
print(check)
