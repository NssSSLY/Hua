# Unicode大小写
text = "HÉLLO WÖRLD ΑΒΓ ПРИВЕТ " * 200
lowered = text.lower()
total = 0
for r in range(10):
    total += len(text.lower().encode())
    if text.casefold() == lowered.casefold():
        total += 1
print(total)
