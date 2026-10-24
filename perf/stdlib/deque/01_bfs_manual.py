# 网格BFS
from collections import deque
W = 300
total = 0
for r in range(3):
    dist = [-1] * (W * W)
    dist[0] = 0
    q = deque([0])
    while q:
        c = q.popleft()
        x, y = c % W, c // W
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < W and 0 <= ny < W:
                n = ny * W + nx
                if dist[n] < 0 and (nx * 7 + ny * 13) % 11 != 0:
                    dist[n] = dist[c] + 1
                    q.append(n)
    total += sum(d for d in dist if d > 0)
print(total)
