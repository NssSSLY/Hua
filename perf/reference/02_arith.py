# 02_arith：范围 for + 整数运算
def main():
    total = 0
    for i in range(0, 2000000):
        total = (total + i * 3) % 1000003
    print(total)
main()
