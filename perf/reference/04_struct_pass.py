# 04_struct_pass：记录按值传入/返回。Hua 结构是值语义，这里显式复制外层与两个内层记录（切片/map 共享，与 Hua 相同）
class Inner:
    __slots__ = ("name", "tags", "count")
    def __init__(self, name, tags, count):
        self.name, self.tags, self.count = name, tags, count
class Outer:
    __slots__ = ("label", "left", "right", "flags", "total")
    def __init__(self, label, left, right, flags, total):
        self.label, self.left, self.right, self.flags, self.total = label, left, right, flags, total
def copy_outer(o):
    return Outer(o.label, Inner(o.left.name, o.left.tags, o.left.count), Inner(o.right.name, o.right.tags, o.right.count), o.flags, o.total)
def bump(o, k):
    out = copy_outer(o)
    out.total = o.total + k
    out.left.count = o.left.count + 1
    return out
def read(o):
    return o.total + o.left.count + o.right.count
def main():
    o = Outer("outer", Inner("l", ["a", "b", "c"], 0), Inner("r", ["x"], 5), {"p": True}, 0)
    acc = 0
    for i in range(0, 200000):
        o = bump(copy_outer(o), i % 7)
        acc = (acc + read(copy_outer(o))) % 1000003
    print(acc, o.left.count)
main()
