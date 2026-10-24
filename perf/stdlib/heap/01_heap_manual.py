# 堆：入堆5万个再全部取出
import heapq
total = 0
for r in range(3):
    h = []
    x = r + 1
    for i in range(50000):
        x = (x * 1103515245 + 12345) % 2147483648
        heapq.heappush(h, x % 1000000)
    k = 0
    while h:
        total += k * heapq.heappop(h) % 1000003
        k += 1
print(total)
