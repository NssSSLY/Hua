# JSONL
import json
text = '{"id":1,"name":"abc"}\n' * 2000
total = 0
for r in range(10):
    for line in text.split("\n"):
        if line:
            total += json.loads(line)["id"]
print(total)
