# 去空白与大小写（参照只含ASCII，与lower_ascii/upper_ascii结果一致）
raw = "   Mixed Case Text With Spaces And Tabs\t "
total = 0
for i in range(100000):
    t = raw.strip(" \t\r\n\v\f")
    total += len(t.lower()) + len(t.upper())
print(total)
