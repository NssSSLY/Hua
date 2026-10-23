# 末次出现位置与出现次数（标准等价：rfind与count）
text = "the quick brown fox jumps over the lazy dog and the cat sleeps by the door"
total = 0
for i in range(150000):
    total += text.rfind("the") + text.count("the") + text.count("o")
print(total)
