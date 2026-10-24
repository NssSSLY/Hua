# 由Unix毫秒拆出UTC分量并格式化为RFC 3339
from datetime import datetime, timedelta, timezone
epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
total = 0
length = 0
for i in range(100000):
    ms = (i * 86399937 * 13) % 4102444800000
    dt = epoch + timedelta(milliseconds=ms)
    total += dt.year + dt.month + dt.day + dt.hour + dt.minute + dt.second + ms % 1000
    text = dt.strftime("%Y-%m-%dT%H:%M:%S") + (f".{ms % 1000:03d}" if ms % 1000 else "") + "Z"
    length += len(text)
print(total, length)
