# 生成UUID v4（只校验格式）
import uuid
ok = 0
for i in range(100000):
    u = str(uuid.uuid4())
    if len(u) == 36 and u[14] == "4" and u[19] in "89ab":
        ok += 1
print(ok)
