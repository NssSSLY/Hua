# 结构日志：手写logfmt格式化写入列表
import time
from datetime import datetime, timezone
def quote(v):
    if " " in v or "=" in v or '"' in v:
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return v
out = []
for i in range(100000):
    ms = int(time.time() * 1000)
    ts = datetime.fromtimestamp(ms / 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S") + (f".{ms % 1000:03d}" if ms % 1000 else "") + "Z"
    out.append("ts=" + ts + " level=info msg=" + quote("request done") + " id=" + str(i) + " path=" + quote("/api/items") + "\n")
print(len(out), "true" if out else "false")
