def go_format(d):
    neg = d < 0
    u = -d if neg else d
    def frac(v, prec):
        out = ""
        printed = False
        for _ in range(prec):
            digit = v % 10
            printed = printed or digit != 0
            if printed:
                out = str(digit) + out
            v //= 10
        if printed:
            out = "." + out
        return out, v
    if u < 1000000000:
        if u == 0:
            return "0s"
        if u < 1000:
            prec, unit = 0, "ns"
        elif u < 1000000:
            prec, unit = 3, "\u00b5s"
        else:
            prec, unit = 6, "ms"
        f, u = frac(u, prec)
        s = str(u) + f + unit
    else:
        f, u = frac(u, 9)
        s = str(u % 60) + f + "s"
        u //= 60
        if u > 0:
            s = str(u % 60) + "m" + s
            u //= 60
            if u > 0:
                s = str(u) + "h" + s
    return ("-" if neg else "") + s
# Go风格时长格式化
total = 0
for i in range(100000):
    d = (i * 7919 * 104729) % 100000000000000 - 5000000000000
    total += len(go_format(d))
print(total)
