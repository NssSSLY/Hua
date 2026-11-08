# S2-2a: scope environments are parked and reused, and Bind writes a slot resolved at load
# time. Every observable behaviour must match the ordinary path. The expected text is what
# the VM produced before the change; the AST interpreter is an independent second opinion.
import sys
from three_path import Cases
with Cases(*sys.argv[1:],'hua-scope-reuse-') as c:
    # Nothing survives from the previous iteration, including a conditional binding.
    c.same('''fn main() {
    for i in 0..3 {
        if i == 0 {
            let x = 5
            print("x", x)
        }
        var y = i * 10
        y += 1
        print(y)
    }
    var n = 0
    while n < 3 {
        let z = n
        n += 1
        print(z)
    }
}
''','x 5\n1\n11\n21\n0\n1\n2\n')
    # A binding later in the block shadows only after it runs, on every iteration.
    c.same('''let y = 100
fn main() {
    for i in 0..3 {
        print(y)
        let y = i
        print(y)
    }
    print(y)
}
''','100\n0\n100\n1\n100\n2\n100\n')
    # Loop variables: key and item, read-only views, `_`, map iteration, nested reuse.
    c.same('''fn main() {
    for i, v in ["a", "b"] { print(i, v) }
    for _ in 0..2 { print("t") }
    for _, v in [7, 8] { print(v) }
    for k, v in map[string]int{"p": 1, "q": 2} { print(k, v) }
    for i in 0..2 {
        for j in 0..2 {
            let t = i * 10 + j
            print(t)
        }
    }
    let f = 1.5
    for x in [0.5, 1.5] {
        let b = x > f - 1.0
        print(x, b)
    }
}
''','0 a\n1 b\nt\nt\n7\n8\np 1\nq 2\n0\n1\n10\n11\n0.5 false\n1.5 true\n')
    # Bound values of every kind are independent per iteration, and mutating one is local.
    c.same('''struct P { x int }
fn main() {
    var kept = [0, 0, 0]
    for i in 0..3 {
        var p = P{x: i}
        p.x += 1
        var a = [i, i]
        a[0] = p.x
        var s = "s" + str(i)
        s = s + "!"
        kept[i] = a[0] * 10 + a[1]
        print(p, a, s)
    }
    print(kept)
}
''','P{x: 1} [1, 0] s0!\nP{x: 2} [2, 1] s1!\nP{x: 3} [3, 2] s2!\n[10, 21, 32]\n')
    # Closures snapshot what they capture, so a reused scope cannot change them.
    c.same('''import std.alg as alg
fn main() {
    var total = 0
    for i in 0..3 {
        let base = i * 100
        fn add(x int) int { return x + base }
        total += add(1)
        print(unwrap(alg.fold([1, 2], 0, add2)))
        print(add(i))
    }
    print(total)
}
fn add2(a int, x int) int { return a + x }
''','3\n0\n3\n101\n3\n202\n303\n')
    # Defers run at scope exit with their captured arguments.
    c.same('''fn note(t string, n int) { print(t, n) }
fn main() {
    for i in 0..2 {
        let v = i + 5
        defer note("defer", v)
        print("body", v)
    }
    print("end")
}
''','body 5\ndefer 5\nbody 6\ndefer 6\nend\n')
    # Assignment targets across a call that opens and closes scopes of the same shape.
    c.same('''fn g(n int) int {
    for i in 0..2 {
        let k = i + n
        if k > 100 { return k }
    }
    return n + 1
}
fn pair(n int) (int, int) { return g(n), g(n + 1) }
fn main() {
    var x = 0
    var y = 0
    for i in 0..3 {
        x = g(i)
        x, y = pair(x)
        print(x, y)
    }
}
''','2 3\n3 4\n4 5\n')
    # Recursion past the parked limit, early return out of nested scopes, then reuse again.
    c.same('''fn deep(n int) int {
    if n == 0 { return 0 }
    let here = n
    return here + deep(n - 1)
}
fn find(limit int) int {
    for i in 0..10 {
        for j in 0..10 {
            let v = i * j
            if v == limit { return i * 100 + j }
        }
    }
    return -1
}
fn main() {
    print(deep(100), deep(40))
    print(find(12), find(81), find(97))
    print(deep(3))
}
''','5050 820\n206 909 -1\n6\n')
    # A runtime error inside nested scopes still reports correctly, and duplicates are errors.
    c.fails('''fn boom(i int) int {
    let d = 0
    return i // d
}
fn main() {
    for i in 0..2 {
        let v = boom(i)
        print(v)
    }
}
''','error[')
    # Reused scopes retain no values: collection finds the same live count each round.
    c.same('''import std.gc as gc
import std.list as list
fn churn() {
    for i in 0..50 {
        var none []int = []
        var l = list.from_slice(none)
        unwrap(list.append(l, i))
        let snapshot = list.snapshot(l)
        print_nothing(snapshot)
    }
}
fn print_nothing(x []int) {}
fn main() {
    churn()
    gc.collect()
    let first = gc.live()
    for r in 0..5 {
        churn()
        gc.collect()
        print(gc.live() == first)
    }
}
''','true\n'*5)
    # Tasks: separate VMs, scopes reused across suspension points.
    c.same('''import std.gc as gc
import std.task as task
fn work(n int) int {
    var total = 0
    for i in 0..n {
        let v = i * 2
        task.yield()
        gc.collect()
        total += v
    }
    return total
}
async fn main() {
    let a = spawn work(5)
    let b = spawn work(3)
    print(await a, await b)
}
''','20 6\n')
print(f'scope reuse checks passed: {c.checks}')
