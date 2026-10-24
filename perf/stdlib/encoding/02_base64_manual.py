# base64编码/解码往返
import base64, zlib
data = bytes((i * 7) % 256 for i in range(4096))
total = 0
for r in range(30):
    text = base64.b64encode(data).decode()
    back = base64.b64decode(text)
    total += len(text) + zlib.crc32(back)
print(total)
