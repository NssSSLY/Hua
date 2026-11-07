a=set(range(5000))
b=set(range(2500,7500))
total=0
for _ in range(20): total+=len(a.symmetric_difference(b))
print(total)
