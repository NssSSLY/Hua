# JSON构造
import json
total = 0
for r in range(20):
    items = [{"id": i, "name": "n" + str(i), "ok": True} for i in range(1000)]
    total += len(json.dumps(items, separators=(",", ":")))
print(total)
