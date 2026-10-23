# 限次切分与限次替换（标准等价：split(sep,1)与replace(a,b,n)）
line = "key=value=with=equals and the the the end"
total = 0
for i in range(150000):
    parts = line.split("=", 1)
    total += len(parts) + len(parts[1])
    total += len(line.replace("the", "THE", 2))
print(total)
