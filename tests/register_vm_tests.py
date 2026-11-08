# R1 typed register execution: every program must behave exactly as on the frame-based
# path (HUA_NO_REGISTER_VM=1): same stdout, same diagnostics with the same "called from"
# traces, same exit code. Cases marked `fast` must actually take the register path, which
# shows as a different budget step count (D4: register instructions are counted).
import json, os, pathlib, re, subprocess, sys, tempfile
hua = pathlib.Path(sys.argv[1]).resolve()
work = pathlib.Path(tempfile.mkdtemp(prefix='hua-register-vm-', dir=sys.argv[3] if len(sys.argv) > 3 else None))
checks = 0
def run(path, frame, extra=()):
    env = dict(os.environ)
    if frame: env['HUA_NO_REGISTER_VM'] = '1'
    else: env.pop('HUA_NO_REGISTER_VM', None)
    p = subprocess.run([str(hua), 'run', str(path), '--report-budget', *extra], capture_output=True, text=True, encoding='utf-8', env=env, timeout=120)
    budget = [l for l in p.stderr.splitlines() if l.startswith('{"schema":"hua.execution.budget')]
    steps = json.loads(budget[-1])['steps'] if budget else None
    stderr = '\n'.join(l for l in p.stderr.splitlines() if not l.startswith('{"schema":"hua.execution.budget'))
    return p.returncode, p.stdout, stderr, steps
def case(name, source, fast=True, extra=()):
    global checks
    path = work / (name + '.hua'); path.write_text(source, encoding='utf-8', newline='\n')
    a = run(path, False, extra); b = run(path, True, extra)
    assert a[:3] == b[:3], (name, 'register', a[:3], 'frame', b[:3])
    if fast: assert a[3] != b[3], (name, 'register path not taken', a[3], b[3])
    checks += 1
    return a

case('loop', '''fn main() {
    var i = 0
    while i < 1000 { i += 1 }
    print(i)
}
''')
case('arith_range', '''fn main() {
    var total = 0
    for i in 0..2000 { total = (total + i * 3) % 1000003 }
    var down = 0
    for j in 10..=0 { down += 1 }
    print(total, down)
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
case('bools', '''fn both(a bool, b bool) bool { return a == b }
fn main() {
    var n = 0
    var flag = false
    while n < 10 && !flag {
        n += 1
        if n == 7 || n > 100 { flag = true }
    }
    print(n, flag, both(true, false), !!flag, flag != false)
}
''')
case('shadow_break', '''fn pick(n int) int {
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
fn main() { print(pick(100), pick(3)) }
''')
case('nested_calls_void', '''fn show(a int, b float, c bool) {
    print(a, b, c)
}
fn twice(x int) int { return x + x }
fn main() {
    show(twice(21), 0.25, twice(2) == 4)
    var i = 0
    while i < 3 {
        show(i, 1.5, i == 1)
        i += 1
    }
}
''')
# Errors: same diagnostic, location and call trace on both paths.
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
case('mod_zero', '''fn main() {
    var z = 0
    print(5 % z)
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
case('float_div_zero', '''fn main() {
    var z = 0.0
    print(1.0 / z)
}
''')
case('add_assign_overflow', '''fn main() {
    var x = 9223372036854775807
    x += 1
    print(x)
}
''')
case('range_zero_step', '''fn main() {
    var n = 0
    for i in 0..10 { n += i }
    print(n)
}
''')
case('depth_limit', '''fn down(n int) int {
    if n == 0 { return 0 }
    return down(n - 1) + 1
}
fn main() { print(down(10), down(100000)) }
''')
case('step_limit', '''fn main() {
    var i = 0
    while true { i += 1 }
}
''', fast=False, extra=('--execution-profile', 'default'))  # both stop at the limit
# Not in the subset (strings, slices, structs): the frame path runs, results unchanged.
case('fallback', '''struct P { x int }
fn get(p P) int { return p.x }
fn label(n int) string { return "n=" + str(n) }
fn main() {
    print(get(P{x: 4}), label(3))
    var xs = [1, 2, 3]
    var s = 0
    for v in xs { s += v }
    print(s)
}
''', fast=False)
case('mixed', '''fn sq(n int) int { return n * n }
fn label(n int) string { return "v" + str(sq(n)) }
fn main() {
    var total = 0
    for i in 0..100 { total += sq(i) }
    print(total, label(7))
}
''', fast=False)
print(f'register VM differential OK ({checks} programs)')
