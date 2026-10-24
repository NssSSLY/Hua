# 洗牌与抽样（输出与排列无关的和）
import random
r = random.Random(7)
l = list(range(10000))
total = 0
for k in range(20):
    r.shuffle(l)
    total += sum(l) + len(r.sample(l, 100))
print(total)
