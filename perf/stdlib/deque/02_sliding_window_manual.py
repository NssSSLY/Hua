# 滑动窗口最大值（单调双端队列）
from collections import deque
xs = []
x = 1
for i in range(100000):
    x = (x * 1103515245 + 12345) % 2147483648
    xs.append(x % 100000)
total = 0
for r in range(3):
    q = deque()
    for i, v in enumerate(xs):
        while q and xs[q[-1]] <= v:
            q.pop()
        q.append(i)
        if q[0] <= i - 50:
            q.popleft()
        if i >= 49:
            total += xs[q[0]]
print(total)
