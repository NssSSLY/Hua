# top-k：20万个取最小100个
import heapq
xs = []
x = 1
for i in range(200000):
    x = (x * 1103515245 + 12345) % 2147483648
    xs.append(x % 10000000)
total = 0
for r in range(5):
    total += sum(heapq.nsmallest(100, xs)) + r
print(total)
