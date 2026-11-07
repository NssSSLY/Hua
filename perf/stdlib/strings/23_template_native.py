# 命名模板
t = "user={name} id={id} city={city} status={status}"
total = 0
for i in range(30000):
    total += len(t.format(name="hua", id=str(i), city="上海", status="active").encode())
print(total)
