a = dict.fromkeys(range(5000), False)
b = dict.fromkeys(range(2500, 7500), True)
total = 0
for _ in range(20):
    result = {}
    for k in a:
        if k not in b: result[k] = True
    for k in b:
        if k not in a: result[k] = True
    total += len(result)
print(total)
