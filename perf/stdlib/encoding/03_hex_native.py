# 十六进制编码/解码往返
import zlib
data = bytes(i % 256 for i in range(4096))
total = 0
for r in range(50):
    text = data.hex()
    back = bytes.fromhex(text)
    total += len(text) + zlib.crc32(back)
print(total)
