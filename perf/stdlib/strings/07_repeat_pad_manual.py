# 手写repeat与左侧补齐（同一算法，不用内建*与rjust）
def repeat_text(text, n):
    out = ""
    for i in range(n):
        out = out + text
    return out
def pad_left(text, width, fill):
    out = text
    while len(out) < width:
        out = fill + out
    return out
total = 0
for i in range(40000):
    total += len(repeat_text("ab", 16))
    total += len(pad_left(str(i), 12, "0"))
print(total)
