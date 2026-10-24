# 大量短消息HMAC-SHA256
import hmac, hashlib
key = b"benchmark-key"
mac = b""
for i in range(5000):
    mac = hmac.new(key, b"message-" + str(i).encode() + mac, hashlib.sha256).digest()
print(mac.hex())
