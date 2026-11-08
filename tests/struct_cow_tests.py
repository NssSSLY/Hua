# V1 copy-on-write structs: copies share one body until a write path makes it private.
# Every observable result must match eager copying. Expected text is what the VM printed
# before the change; the AST interpreter (always eager) is an independent second opinion.
import sys
from three_path import Cases
with Cases(*sys.argv[1:],'hua-struct-cow-') as c:
    c.same(r'''import std.list as list
struct In { n int
    tags []string }
struct S { a In
    k int
    label string }
fn give(s S) S { return s }
fn read(s S) int { return s.k + s.a.n }
fn wrap(s S) Result<S, string> { return ok(s) }
fn main() {
    var x = S{a: In{n: 1, tags: ["t"]}, k: 2, label: "x"}
    var y = x
    y.k = 20
    y.a.n = 10
    print(x, y)
    var z = give(x)
    z.a.n += 5
    x.a.n += 100
    print(x.a.n, z.a.n, read(x), read(z))
    var w = unwrap(wrap(x))
    w.label = "w"
    print(x.label, w.label)
    var none []S = []
    var l = list.from_slice(none)
    unwrap(list.append(l, x))
    x.k = 99
    var got = unwrap(list.get(l, 0))
    print(got.k, x.k)
    var m = map[string]S{"p": x}
    x.k = 7
    print(m["p"])
    var xs = [x, x]
    xs[0].k = 1
    xs[1].a.n = 2
    print(x.k, x.a.n, xs[0].k, xs[1].a.n, xs[0].a.n)
}
''','S{a: In{n: 1, tags: [t]}, k: 2, label: x} S{a: In{n: 10, tags: [t]}, k: 20, label: x}\n101 6 103 8\nx w\n2 99\nS{a: In{n: 101, tags: [t]}, k: 99, label: x}\n7 101 1 2 101\n')
    c.same(r'''struct P { v int }
struct Row { cells [3]P
    name string }
struct Grid { rows []Row
    first Row }
fn main() {
    var r = Row{cells: [P{v: 1}, P{v: 2}, P{v: 3}], name: "r"}
    var g = Grid{rows: [r, r], first: r}
    var h = g
    h.first.cells[1].v = 50
    h.first.name = "h"
    g.rows[0].cells[0].v = 9
    print(g.first, h.first)
    print(g.rows[0].cells[0].v, h.rows[0].cells[0].v, r.cells[0].v)
    var s = h.first.cells[:]
    s[2].v = 77
    var k = h
    s[2].v = 88
    print(h.first.cells[2].v, k.first.cells[2].v)
}
''','Row{cells: [P{v: 1}, P{v: 2}, P{v: 3}], name: r} Row{cells: [P{v: 1}, P{v: 50}, P{v: 3}], name: h}\n9 9 1\n88 77\n')
    c.same(r'''struct In { n int }
struct S { a In
    b In
    k int }
var g = S{a: In{n: 1}, b: In{n: 2}, k: 0}
fn same(s S) S { return s }
fn bump() int { g.k += 100
    return g.k }
fn swap(s S) In { return s.a }
fn main() {
    var x = S{a: In{n: 1}, b: In{n: 2}, k: 0}
    x.a = same(x).b
    print(x)
    x.b = swap(x)
    print(x)
    x.k = same(x).k + 1
    print(x)
    var keep = x
    x.a.n, x.k = same(x).k, keep.a.n + 1
    print(x, keep)
    g.a.n = bump()
    print(g)
    g.k = same(g).a.n
    print(g)
}
''','S{a: In{n: 2}, b: In{n: 2}, k: 0}\nS{a: In{n: 2}, b: In{n: 2}, k: 0}\nS{a: In{n: 2}, b: In{n: 2}, k: 1}\nS{a: In{n: 1}, b: In{n: 2}, k: 3} S{a: In{n: 2}, b: In{n: 2}, k: 1}\nS{a: In{n: 100}, b: In{n: 2}, k: 100}\nS{a: In{n: 100}, b: In{n: 2}, k: 100}\n')
    c.same(r'''struct C { n int
    hist []int }
fn grow(c mut C) { c.n += 1 }
fn pair(a C, b mut C) int { b.n += 10
    return a.n }
fn C.inc() { self.n += 1 }
fn C.get() int { return self.n }
fn main() {
    var c = C{n: 0, hist: []}
    var copy = c
    grow(c)
    grow(c)
    print(c.n, copy.n)
    print(pair(c, c), c.n)
    let before = c
    c.inc()
    print(before.n, c.n, c.get())
    let f = c.inc
    var d = c
    f()
    f()
    print(c.n, d.n)
    d.inc()
    print(c.n, d.n)
}
''','2 0\n2 12\n12 13 13\n15 13\n15 14\n')
    c.same(r'''struct In { n int
    xs []int }
struct S { a In
    xs []int
    k int }
fn edit(s S) S {
    var t = s
    t.k = 5
    t.a.n = 6
    return t
}
fn main() {
    let x = S{a: In{n: 1, xs: [1]}, xs: [2], k: 0}
    var t = edit(x)
    print(x, t)
    var u = x
    u.a.n = 9
    print(u.a.n, x.a.n)
}
''','S{a: In{n: 1, xs: [1]}, k: 0, xs: [2]} S{a: In{n: 6, xs: [1]}, k: 5, xs: [2]}\n9 1\n')
    c.static(r'''struct S { xs []int
    k int }
fn edit(s S) {
    var t = s
    t.xs[0] = 5
}
fn main() {
    edit(S{xs: [1], k: 0})
}
''','error[E3003]')
    c.static(r'''struct In { xs []int }
struct S { a In }
fn edit(s S) {
    var t = s
    t.a.xs[0] = 5
}
fn main() {
    edit(S{a: In{xs: [1]}})
}
''','error[E3003]')
    c.same(r'''struct In { n int }
enum E { A(In), B }
fn show(e E) string {
    var out = "?"
    match e { A(x) => out = "A" + str(x.n)
        B => out = "B" }
    return out
}
fn note(s In) { print("defer", s.n) }
fn main() {
    var i = In{n: 1}
    let e = E.A(i)
    i.n = 2
    print(show(e), i.n)
    var acc = [i, i, i]
    for k in 0..3 {
        var cur = i
        cur.n += k
        defer note(cur)
        acc[k] = cur
        i.n = cur.n * 10
    }
    print(acc, i)
    let base = i
    fn look() int { return base.n }
    i.n = 0
    print(look(), i.n)
}
''','A1 2\ndefer 2\ndefer 21\ndefer 212\n[In{n: 2}, In{n: 21}, In{n: 212}] In{n: 2120}\n2120 0\n')
    c.fails(r'''struct S { k int
    v int }
fn boom(s S) int { return s.k // s.v }
fn main() {
    var x = S{k: 1, v: 0}
    var y = x
    x.k = boom(x)
    print(y)
}
''','error[E4004]')
    c.same(r'''struct In { n int
    s string }
struct S { a In
    k int }
fn pass(s S) S { let t = s
    return t }
fn main() {
    var x = S{a: In{n: 1, s: "long string that is not inline at all"}, k: 0}
    var a = pass(pass(x))
    var b = a
    var c = [a, b, pass(b)]
    c[0].k = 1
    b.k = 2
    c[2].a.n = 3
    a.a.s = "a"
    x.a.n = 4
    var d = c
    d[1].k = 5
    print(x, a, b)
    print(c)
    print(d)
}
''','S{a: In{n: 4, s: long string that is not inline at all}, k: 0} S{a: In{n: 1, s: a}, k: 0} S{a: In{n: 1, s: long string that is not inline at all}, k: 2}\n[S{a: In{n: 1, s: long string that is not inline at all}, k: 1}, S{a: In{n: 1, s: long string that is not inline at all}, k: 5}, S{a: In{n: 3, s: long string that is not inline at all}, k: 0}]\n[S{a: In{n: 1, s: long string that is not inline at all}, k: 1}, S{a: In{n: 1, s: long string that is not inline at all}, k: 5}, S{a: In{n: 3, s: long string that is not inline at all}, k: 0}]\n')
    c.same(r'''import std.task as task
import std.gc as gc
struct S { k int
    xs []int }
fn work(s S) S { var t = s
    t.k += 1
    task.yield()
    return t }
async fn main() {
    var x = S{k: 1, xs: [1]}
    var y = x
    let h = spawn work(x)
    x.k = 100
    let r = await h
    gc.collect()
    print(x.k, y.k, r.k)
}
''','100 1 2\n')
    c.same(r'''struct C { n int }
fn grow(c mut C) int { c.n += 1
    return c.n }
fn show(a C, n int) string { return str(a.n) + "/" + str(n) }
fn main() {
    var c = C{n: 0}
    print(show(c, grow(c)))
    var keep = c
    print(show(c, grow(c)), keep.n)
}
''','1/1\n2/2 1\n')
    c.same(r'''struct In { n int }
struct C { inner In
    k int }
var g = C{inner: In{n: 0}, k: 0}
fn grow(c mut C) int { c.inner.n += 1
    return c.inner.n }
fn show(a In, n int) string { return str(a.n) + "/" + str(n) }
fn touch() C { g.inner.n += 5
    return g }
fn two(c mut C) (In, int) { return c.inner, grow(c) }
fn main() {
    var c = C{inner: In{n: 0}, k: 0}
    var keep = c
    print(show(c.inner, grow(c)), keep.inner.n)
    let snap = g
    print(g == touch(), snap.inner.n)
    var d = c
    let a, b = two(c)
    print(a.n, b, d.inner.n, c.inner.n)
}
''','1/1 0\ntrue 0\n2 2 1 2\n')
print(f'struct cow checks passed: {c.checks}')
