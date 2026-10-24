# 结构日志：手写logfmt格式化，每行一次写stderr
import sys, time
from datetime import datetime, timezone
lines = 0
for i in range(100000):
    ms = int(time.time() * 1000)
    ts = datetime.fromtimestamp(ms / 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S") + (f".{ms % 1000:03d}" if ms % 1000 else "") + "Z"
    sys.stderr.write("ts=" + ts + ' level=info msg="request done" id=' + str(i) + " path=/api/items\n")
    sys.stderr.flush()
    lines += 1
print(lines, "true")
