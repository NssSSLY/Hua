# 网格Dijkstra（键编码为dist*100000+node）
import heapq
W = 150
total = 0
for r in range(2):
    dist = [10**18] * (W * W)
    dist[0] = 0
    h = [0]
    while h:
        e = heapq.heappop(h)
        d, c = e // 100000, e % 100000
        if d > dist[c]:
            continue
        x, y = c % W, c // W
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < W and 0 <= ny < W:
                n = ny * W + nx
                nd = d + (nx * 31 + ny * 17) % 9 + 1
                if nd < dist[n]:
                    dist[n] = nd
                    heapq.heappush(h, nd * 100000 + n)
    total += sum(dist)
print(total)
