# 端序打包
import struct
total = 0
for i in range(30000):
    v = i * 7919
    total += struct.unpack(">I", struct.pack(">I", v))[0]
    n = (i % 65536) - 32768
    total += struct.unpack("<h", struct.pack("<h", n))[0]
print(total)
