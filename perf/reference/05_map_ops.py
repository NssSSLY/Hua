# 05_map_ops：dict[str,bool] 插入、查询、遍历
def main():
    m = {}
    for i in range(0, 20000):
        m["k" + str(i)] = i % 2 == 0
    hits = 0
    for round in range(0, 20):
        for i in range(0, 20000):
            if ("k" + str(i * 7 % 20000)) in m and m["k" + str(i)]:
                hits += 1
    count = 0
    for key, flag in m.items():
        if flag:
            count += 1
    print(hits, count)
main()
