# Native backend prototype: every program is compiled to C++ and must behave exactly as
# on the VM: same stdout, same stderr (diagnostic text, location, source excerpt and
# "called from" trace) and same exit code. Programs outside the subset must be rejected
# by `hua native` with exit code 2, never miscompiled.
import concurrent.futures, pathlib, subprocess, sys, tempfile, shutil
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'tools'))
import native_build
hua = pathlib.Path(sys.argv[1]).resolve()
work = pathlib.Path(tempfile.mkdtemp(prefix='hua-native-', dir=sys.argv[3] if len(sys.argv) > 3 else None))
shutil.copy(hua.parent / 'wasmtime.dll', work / 'wasmtime.dll')
cases = {}
def case(name, source): cases[name] = source
rejected = {}
def reject(name, source): rejected[name] = source

case('loop', '''fn main() {
    var i = 0
    while i < 1000 { i += 1 }
    print(i)
}
''')
case('ranges', '''fn main() {
    var total = 0
    for i in 0..2000 { total = (total + i * 3) % 1000003 }
    var down = 0
    for j in 10..=0 { down += 1 }
    var inc = 0
    for k in 0..=5 { inc += k }
    print(total, down, inc)
}
''')
case('fib', '''fn fib(n int) int {
    if n < 2 { return n }
    return fib(n-1) + fib(n-2)
}
fn main() { print(fib(15)) }
''')
case('int_ops', '''fn f(a int, b int) int { return a // b * 1000 + a % b }
fn main() {
    print(f(7, 2), f(-7, 2), f(7, -2), f(-7, -2))
    print(7 / 2, -7 // 2, -7 % 3, 2 ** 10)
    print(1 < 2, 2 <= 2, 3 > 4, 4 >= 5, 5 == 5, 5 != 6)
    let m = -9223372036854775807 - 1
    print(m, m + 1, -(m + 1))
}
''')
case('float_ops', '''fn h(x float, y float) float { return x * y + x / y - y }
fn main() {
    var x float = 1
    x += 0.5
    print(h(x, 2.0), h(-3.25, 0.5), x < 2.0, x == 1.5, -x)
    let k float = 3
    print(k / 2.0, k * 2.5 - 1.0)
}
''')
case('bools_shadow', '''fn both(a bool, b bool) bool { return a == b }
fn pick(n int) int {
    var found = -1
    for i in 0..n {
        let n2 = i * i
        if n2 > 50 {
            found = i
            break
        }
        if i % 2 == 0 { continue }
    }
    {
        let found2 = found + 1
        found = found2
    }
    return found
}
fn main() {
    var n = 0
    var flag = false
    while n < 10 && !flag {
        n += 1
        if n == 7 || n > 100 { flag = true }
    }
    print(n, flag, both(true, false), !!flag, flag != false, pick(100), pick(3))
}
''')
case('structs', '''struct In { n int
    tags []string }
struct S { a In
    k int
    label string }
fn read(s S) int { return s.k + s.a.n + len(s.a.tags) }
fn make(n int) S { return S{a: In{n: n, tags: ["x", "y"]}, k: n * 2, label: "s" + str(n)} }
fn bump(s S) S {
    var out = s
    out.k += 1
    out.a.n = s.a.n * 10
    return out
}
fn main() {
    var x = make(3)
    let y = x
    x.k = 100
    let z = bump(x)
    print(read(x), read(y), read(z), x.label, y.a.tags, x.a.n == y.a.n)
    print(x, z)
}
''')
case('strings_slices', '''import std.strings as strings
fn words(text string) int {
    var n = 0
    for w in strings.split(text, " ") {
        if strings.starts_with(w, "a") || len(w) > 4 { n += 1 }
    }
    return n
}
fn total(xs []int) int {
    var s = 0
    for i, v in xs { s += v * i }
    return s + xs[0] + xs[len(xs) - 1]
}
fn main() {
    print(words("an apple and a banana tree"), total([3, 1, 4, 1, 5]))
    let parts = ["a", "b"]
    var ys = [1, 2, 3]
    ys[1] = 20
    ys[2] += 5
    print(parts[1] + parts[0], strings.replace("aXbX", "X", "-"), ys, strings.ends_with("abc", "bc"))
}
''')
case('maps', '''fn count(words []string) map[string]int {
    var m = map[string]int{}
    for w in words {
        if has(m, w) { m[w] += 1 } else { m[w] = 1 }
    }
    return m
}
fn main() {
    let m = count(["b", "a", "b", "c", "b"])
    var keys = ""
    var sum = 0
    for k, v in m {
        keys = keys + k
        sum += v
    }
    var ids = map[int]bool{7: true, 2: false}
    ids[5] = true
    var order = 0
    for id in ids { order = order * 10 + id }
    let copy = clone(m)
    print(m, keys, sum, m["b"], m["zz"], order, len(copy), unwrap_or(m["q"], -1))
}
''')
case('results', '''fn parse(n int) Result<int> {
    if n < 0 { return err("negative") }
    return ok(n * 10)
}
fn twice(n int) Result<int> {
    let a = parse(n)?
    let b = parse(a - 5)?
    return ok(a + b)
}
fn main() {
    print(twice(2), twice(-1), twice(0), is_ok(twice(1)), unwrap_or(twice(-3), 7), is_err(parse(-1)))
}
''')
case('lists', '''import std.list as list
fn fill(xs mut List<int>, n int) {
    for i in 0..n { list.append(xs, i * i) }
}
fn sum(xs List<int>) int {
    var s = 0
    for v in list.snapshot(xs) { s += v }
    return s
}
fn main() {
    var xs = list.from_slice([1])
    fill(xs, 5)
    print(list.len(xs), sum(xs), unwrap(list.get(xs, 2)), list.get(xs, 99))
}
''')
case('mut_struct', '''struct Counter { n int
    name string }
fn tick(c mut Counter, by int) { c.n += by }
fn main() {
    var c = Counter{n: 1, name: "c"}
    tick(c, 4)
    tick(c, 5)
    print(c.n, c)
}
''')
case('main_err', '''fn main() Result<void> {
    print("before")
    return err("bad exit")
}
''')
# Failures: same diagnostic, location, excerpt and call trace.
case('overflow_trace', '''fn grow(n int) int { return n * 3037000500 }
fn outer(n int) int { return grow(n) + 1 }
fn main() { print(outer(3037000500)) }
''')
case('div_zero', '''fn d(a int, b int) int { return a // b }
fn main() {
    print(d(10, 3))
    print(d(1, 0))
}
''')
case('neg_min', '''fn neg(x int) int { return -x }
fn main() {
    let m = -9223372036854775807 - 1
    print(neg(m))
}
''')
case('float_overflow', '''fn big(x float) float { return x * x }
fn main() { print(big(1e200)) }
''')
case('add_assign_overflow', '''fn main() {
    var x = 9223372036854775807
    x += 1
    print(x)
}
''')
case('index_oob', '''fn pick(xs []int, i int) int { return xs[i] }
fn outer(xs []int) int { return pick(xs, 5) + 1 }
fn main() { print(outer([1, 2, 3])) }
''')
case('unwrap_nil', '''fn get(m map[string]int, k string) int { return unwrap(m[k]) + 1 }
fn main() {
    let m = map[string]int{"a": 1}
    print(get(m, "a"))
    print(get(m, "zz"))
}
''')
case('unwrap_err', '''fn bad() Result<int> { return err("boom") }
fn use(n int) int { return unwrap(bad()) + n }
fn main() { print(use(1)) }
''')
case('depth_limit', '''fn down(n int) int {
    if n == 0 { return 0 }
    return down(n - 1) + 1
}
fn main() { print(down(10), down(100000)) }
''')
# Outside the subset: rejected, not miscompiled.
reject('function_value', '''fn twice(x int) int { return x * 2 }
fn main() {
    let f = twice
    print(f(2))
}
''')
case('defer', '''fn note(text string, n int) { print(text, n) }
fn work(n int) int {
    defer note("leave", n)
    for i in 0..3 {
        defer note("iteration", i)
        if i == 1 { break }
    }
    if n > 5 { return n // 0 }
    return n + 1
}
fn main() {
    print(work(1))
    print(work(9))
}
''')
reject('task', '''fn work(n int) int { return n * 2 }
fn main() {
    let t = spawn work(21)
    print(await t)
}
''')
case('methods', '''import std.buffer as buffer
struct Box { n int
    out Buffer }
fn Box.double() int { return self.n * 2 }
fn Box.bump(k int) { self.n += k }
fn Box.twice() int { return self.double() + self.double() }
fn main() {
    var b = Box{n: 2, out: buffer.new()}
    b.bump(5)
    buffer.write_int(b.out, b.double())
    let c = b
    b.bump(1)
    print(b.double(), c.twice(), buffer.len(b.out), buffer.len(c.out))
}
''')
case('enums_match', '''enum Shape { Circle(int), Named(string), Pair(int, string), Empty }
fn area(s Shape) int {
    match s {
        Circle(r)=>{return r * r}
        Named(t)=>{return len(t)}
        Pair(a, b)=>{return a + len(b)}
        _=>{return 0}
    }
}
fn word(n int) string {
    match n {
        1=>{return "one"}
        2=>{return "two"}
        _=>{return "many"}
    }
}
fn main() {
    let a = Shape.Circle(3)
    var total = 0
    for i in 0..4 {
        match i {
            0=>{total += area(a)}
            1=>{total += area(Shape.Named("xy"))}
            2=>{
                total += area(Shape.Pair(4, "abc"))
                continue
            }
            _=>{total += area(Shape.Empty())}
        }
        total += 100
    }
    print(total, word(1), word(2), word(7), a, Shape.Pair(1, "z"), a == Shape.Circle(3), a == Shape.Empty())
}
''')

def run(command):
    p = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=120)
    return p.returncode, p.stdout, p.stderr
def check(item):
    name, source = item
    path = work / (name + '.hua'); path.write_text(source, encoding='utf-8', newline='\n')
    code, message = native_build.build(hua, path, work / (name + '.exe'))
    if code: return name, 'build failed (%d): %s' % (code, message[-1500:])
    vm = run([str(hua), 'run', str(path), '--execution-profile', 'compiler']); native = run([str(work / (name + '.exe'))])
    return name, None if vm == native else 'VM %r\nnative %r' % (vm, native)
def refuse(item):
    name, source = item
    path = work / (name + '.hua'); path.write_text(source, encoding='utf-8', newline='\n')
    if run([str(hua), 'check', str(path)])[0]: return name, 'test program does not type-check'
    code, message = native_build.build(hua, path, work / (name + '.exe'))
    return name, None if code == 2 and 'unsupported' in message else 'expected rejection, got %d: %s' % (code, message[-600:])
failures = []
with concurrent.futures.ThreadPoolExecutor(6) as pool:
    for name, problem in list(pool.map(check, cases.items())) + list(pool.map(refuse, rejected.items())):
        if problem: failures.append((name, problem))
for name, problem in failures: print('FAIL', name, '\n', problem)
if failures: sys.exit(1)
shutil.rmtree(work, ignore_errors=True)
print(f'native backend differential OK ({len(cases)} programs identical to the VM, {len(rejected)} rejected)')
