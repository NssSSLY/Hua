# 按结构字段排序
class Item:
    def __init__(self, id, score):
        self.id = id
        self.score = score
items = [Item(i, (i * 7919) % 1000) for i in range(5000)]
check = 0
for r in range(10):
    ordered = sorted(items, key=lambda it: it.score)
    for j in range(0, 5000, 97):
        check += j * ordered[j].id
print(check)
