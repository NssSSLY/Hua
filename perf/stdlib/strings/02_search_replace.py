# 查找与替换
text = "the quick brown fox jumps over the lazy dog and the cat sleeps"
total = 0
for i in range(150000):
    if "lazy" in text:
        total += 1
    total += text.find("cat")
    changed = text.replace("the", "THE")
    total += len(changed)
print(total)
