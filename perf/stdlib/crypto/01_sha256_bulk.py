# SHA-256批量（1 MiB×8轮）
import hashlib
data = bytes((i * 31) % 256 for i in range(1 << 20))
d = b""
for r in range(8):
    d = hashlib.sha256(data + d).digest()
print(d.hex())
