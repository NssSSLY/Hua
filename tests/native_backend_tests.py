# Native backend prototype: every program is compiled to C++ and must behave exactly as
# on the VM: same stdout, same stderr (diagnostic text, location, source excerpt and
# "called from" trace) and same exit code. Programs outside the subset must be rejected
# by `hua native` with exit code 2, never miscompiled.
import concurrent.futures, os, pathlib, subprocess, sys, tempfile, shutil
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
case('function_value', '''fn twice(x int) int { return x * 2 }
fn main() {
    let f = twice
    print(f(2))
}
''')
case('call_argument_order', '''struct Counter { n int }
var integer = 4
var values = [5]
fn scalar_change() int { integer = 7
    return integer }
fn index_change() int { values = [20]
    return 0 }
fn change_record(c mut Counter) int { c.n += 1
    return c.n }
fn pair(a int, b int) { print(a,b) }
fn record(a Counter, b int) { print(a.n,b) }
fn main() {
    pair(integer, scalar_change())
    print(values[index_change()], values[0])
    var c = Counter{n: 0}
    record(c, change_record(c))
}
''')
case('multiple_values', '''struct Counter { n int }
fn bump(c mut Counter) int { c.n += 1
    return c.n }
fn two(c mut Counter) (Counter,int) { return c,bump(c) }
fn relay(c mut Counter) (Counter,int) { return two(c) }
fn main() {
    var c = Counter{n:4}
    let before,n = relay(c)
    var x,y = n,7
    x,y = y,x
    let _,z = relay(c)
    print(before.n,n,x,y,z,c.n)
}
''')
case('typed_callbacks_and_capture', '''import std.alg as alg
var base = 1
fn larger(a int,b int) bool { print(a,b)
    return a>b }
fn main() {
    var offset = 3
    fn mapper(x int) int { return x+offset+base }
    fn reduce(a int,x int) int { return a+x }
    offset = 8
    base = 5
    let xs = [2,1,2,3]
    let ys = alg.map(xs,mapper)
    let alias = mapper
    print(ys, alg.fold(ys,0,reduce), alias(5))
    print(alg.sorted_by(xs,larger))
}
''')
case('callback_diagnostic', '''import std.alg as alg
fn inverse(x int) int { return 1 // x }
fn main() { print(alg.map([1,0], inverse)) }
''')
case('escaping_closure', '''fn outer(x int) {
    fn recur(n int) int {
        if n==0 { return x }
        return recur(n-1)
    }
    return recur
}
fn main() {
    let first=outer(17)
    let second=outer(32)
    print(first(3),second(2),first(1))
}
''')
case('typed_locations_hold_old_objects', """struct Inner { n int }
struct Box { inner Inner }
var root=Box{inner:Inner{n:1}}
var xs=[10,20]
var m=map[int]int{0:1}
fn replace_all() (int,int,int) {
    root=Box{inner:Inner{n:9}}
    xs=[30,40]
    m=map[int]int{0:99}
    return 2,3,4
}
fn main() {
    var keep=xs
    var old=m
    root.inner.n,xs[0],m[0]=replace_all()
    print(root.inner.n,xs,keep,m[0],old[0])
    root.inner,root.inner.n=Inner{n:8},7
    print(root.inner.n)
    old[2],old[2],keep[1],_=5,6,21,4
    print(old[2],keep)
}
""")
case('local_field_after_mutating_rhs', """struct Counter { n int }
var external=Counter{n:100}
fn replace_external() int {
    external=Counter{n:30}
    return 7
}
fn change(c mut Counter) int {
    c.n,c.n=4,5
    return 3
}
fn main() {
    var c=Counter{n:1}
    c.n+=change(c)
    external.n=replace_external()
    print(c.n,external.n)
}
""")
case('locations_after_map_rehash', """var m=map[int]int{0:1}
fn rhs() (int,int) {
    for i in 1..300 { m[i]=i }
    return 7,8
}
fn main() {
    m[0],m[400]=rhs()
    print(m[0],m[400],m.len)
}
""")
case('locations_map_capacity_failure', """var m=map[int]int{}
var stored=1
fn main() {
    for i in 0..999999 { m[i]=i }
    defer report()
    stored,m[-1],m[-2]=9,3,4
}
fn report() { print(stored,m[-1],m[-2],m.len) }
""")
case('compound_capture_snapshot', """import std.list as list
struct Box { a [2]int
    xs []int }
fn make() {
    var xs=[1,2]
    var box=Box{a:[3,4],xs:xs}
    var rows=[box]
    var lookup=map[int]Box{0:box}
    var items=list.from_slice(rows)
    fn read() int {
        return box.a[0]+box.xs[0]+rows[0].xs[0]+unwrap(lookup[0]).xs[0]+unwrap(list.get(items,0)).xs[0]
    }
    box.a[0]=7
    xs[0]=9
    xs=[50,60]
    return read
}
fn main() { let saved=make()
    print(saved())
}
""")
case('indirect_function_and_defer', """fn chooser(x int) {
    fn a(n int) int {return x+n}
    fn b(n int) int {return x-n}
    if x>0 {return a}
    return b
}
fn choose_output(x int) {
    fn plus(n int) {print(x+n)}
    fn minus(n int) {print(x-n)}
    if x>0 {return plus}
    return minus
}
fn main() {
    let f=chooser(3)
    let g=chooser(-3)
    let v int=f(2)
    print(v,g(2))
    var later=choose_output(3)
    defer later(2)
    later=choose_output(10)
    later(2)
    later=choose_output(-3)
}
""")
case('indirect_invalid_arity_order', """fn side() int { print("evaluated")
    return 2
}
fn select(x int) {
    fn one() {print(1)}
    fn two() {print(2)}
    if x>0 {return one}
    return two
}
fn main() { let f=select(1)
    f(side())
}
""")
case('dynamic_functions_with_different_signatures', """fn choose(eq bool) {
    fn a(n int) int {return n+2}
    fn b(n string) string {return n+"!"}
    if eq {return a}
    return b
}
fn main() {
    let f=choose(true)
    let g=choose(false)
    let v int=f(2)
    let w string=g("a")
    print(v,w,f(3))
}
""")
case('dynamic_argument_error_order', """fn make(use_int bool) {
    fn a(a int,b int) int { return a+b }
    fn b(a string,b int) string { return a+str(b) }
    if use_int {return a}
    return b
}
fn effect() int {print("right")
    return 7
}
fn main() {
    let f=make(true)
    f("bad",effect())
}
""")
case('closure_and_recursive_storage_collection', """import std.gc as gc
struct Node { children []Node }
fn make() {
    var nodes=[Node{children:[]}]
    nodes[0].children=nodes
    fn read() int {return len(nodes[0].children)}
    return read
}
fn live() {
    let closure=make()
    print(gc.collect()==0,closure())
}
fn main() {
    live()
    print(gc.collect()>0)
    print(gc.collect()==0)
}
""")
case('captured_interface_snapshot', """interface Readable { fn read() int }
struct Box { offset int
    xs []int }
fn Box.read() int {return self.offset+self.xs[0]}
fn main() {
    var xs=[2]
    var b=Box{offset:1,xs:xs}
    var v Readable=b
    fn read() int {return v.read()}
    b.offset=90
    xs[0]=3
    print(read())
}
""")
case('function_values_in_slice', """fn make(x int) {
    fn f() int {return x}
    return f
}
fn main(){
    var callbacks=[make(1),make(2)]
    let before=callbacks[0]
    callbacks[0],callbacks[1]=callbacks[1],callbacks[0]
    print(before(),callbacks[0](),callbacks[1]())
}
""")
case('n1_short_closure_aliases_and_defer', '''fn make(value int) {
    fn read() int {print(value)
        return value}
    return read
}
fn main(){
    let short=make(7)
    defer short()
    for i in 0..3 {
        let pair=[make(i),make(i+1)]
        let here=pair[1]
        let alias=here
        alias()
    }
}
''')
case('n1_short_closure_index_failure', '''fn make(value int) {
    fn read() int {return value}
    return read
}
fn select(i int) int {
    let pair=[make(1),make(2)]
    let f=pair[i]
    return f()
}
fn main(){ print(select(2)) }
''')
case('n1_stack_and_escaping_closure_same_factory', '''fn make(value int) {
    fn read() int {return value}
    return read
}
fn escaped() {
    let pair=[make(10),make(21)]
    return pair[1]
}
fn main(){
    let transient=make(4)
    let stored=escaped()
    var outside=[make(0)]
    for i in 0..4 {
        let pair=[make(i),make(i+1)]
        outside[0]=pair[1]
    }
    print(transient(),stored(),outside[0]())
}
''')
case('n1_indirect_target_saved_before_rebind', '''fn select(base int) {
    fn plus(n int) int {return base+n}
    fn minus(n int) int {return base-n}
    if base>0 {return plus}
    return minus
}
var selected=select(3)
fn replace() int {
    selected=select(100)
    return 2
}
fn main(){ print(selected(replace()),selected(2)) }
''')
case('n1_indirect_two_targets_keep_diagnostic_trace', '''fn choose(base int) {
    fn divide(n int) int {return 9//(base+n)}
    fn other(n int) int {return base+n}
    if base>0 {return divide}
    return other
}
fn main(){
    let f=choose(1)
    print(f(-1))
}
''')
case('n1_indirect_target_can_save_itself', '''fn id(f) {return f}
fn choose(base int) {
    fn first(x int) int {
        if x==1 { saved=id(first) }
        return base
    }
    fn second(x int) int {return x-base}
    if base>0 {return first}
    return second
}
var saved=id(choose(3))
fn call_then_leave(){
    let f=choose(73)
    f(1)
}
fn main(){ call_then_leave()
    print(saved(0))
}
''')
case('n1_apply', '''fn apply(f,x) {return f(x)}
fn plus(x int) int {return x+1}
fn twice(x int) int {return x*2}
fn main(){ let a int=apply(plus,4)
 let b int=apply(twice,4)
 print(a,b)
}
''')
case('n1_mixed', '''struct Box { n int }
fn set(x mut Box) Box { x.n+=3
 return x }
fn collect(x []int) []int { return x }
fn choose(a bool) { if a {return set}
 return collect }
fn main() { var box=Box{n:1}
 let f=choose(true)
 let out Box=f(box)
 var xs=[1,2]
 let g=choose(false)
 var ys []int=g(xs)
 xs[0]=5
 print(box,out,ys)
}
''')
case('n1_fixed_borrow', '''fn bump(xs mut []int) int {xs[0]+=5
 return xs[0]}
fn other(n int) string {return str(n)}
fn choose(b bool) {if b {return bump}
 return other}
fn main(){var xs [2]int=[1,2]
 let f=choose(true)
 let v int=f(xs)
 print(v,xs)
}
''')
case('n1_mut_readonly', '''struct Box { n int }
fn set(x mut Box,n int) int {x.n=n
 return x.n}
fn other(s string,n int) string {return s+str(n)}
fn choose(ok bool) {if ok {return set}
 return other}
fn rhs() int {print("right")
 return 2}
fn main(){let box=Box{n:1}
 let f=choose(true)
 f(box,rhs())
}
''')
case('n1_multi_result', '''fn pair(n int) (int,string) {return n,"x"}
fn triple(x string) (string,int,int) {return x,1,2}
fn choose(b bool) {if b {return pair}
 return triple}
fn main(){
 let f=choose(true)
 let x int,s string=f(4)
 let g=choose(false)
 let t,n,m=g("y")
 print(x,s,t,n,m)
}
''')
case('n1_mixed_container', '''fn a(n int) int {return n+1}
fn b(n int) int {return n*2}
fn choose(q bool) { if q {return a}
 return b }
fn main() { var xs=[choose(true),choose(false)]
 print(xs[0](5))
}
''')
case('n1_bound_index', '''struct Box { n int }
fn Box.next() int {self.n+=1
 return self.n}
fn main(){ var a=[Box{n:4}]
 let f=a[0].next
 a[0]=Box{n:100}
 print(f(),a[0].n)
}
''')
case('n1_bound_interface_return', '''interface Step { mut fn next() int }
struct Box { n int }
fn Box.next() int {self.n+=1
 return self.n}
fn make(a mut Step) { return a.next }
fn invoke(f) {return f()}
fn main(){ var a Step=Box{n:4}
 let f=make(a)
 a=Box{n:100}
 let n int=invoke(f)
 print(n,a.next())
}
''')
case('n1_bound_interface_arg', '''interface Step { mut fn next() int }
struct Box { n int }
fn Box.next() int {self.n+=1
 return self.n}
fn invoke(f) {return f()}
fn main(){ var a Step=Box{n:4}
 let n int=invoke(a.next)
 print(n,a.next())
}
''')
case('n1_bound_interface_valid', '''interface Step { mut fn next() int }
struct Box { n int }
fn Box.next() int {self.n+=1
 return self.n}
fn invoke(f) {return f()}
fn main(){ var a Step=Box{n:4}
 let f=a.next
 a=Box{n:100}
 let n int=invoke(f)
 print(n,a.next())
}
''')
case('n1_file_capture_after_return', '''import std.file as file
import std.fs as fs
fn reader(path string) {
    let h=unwrap(file.open(path,"r"))
    fn next() string {
        let answer=unwrap_or(unwrap(file.read_line(h)),"eof")
        unwrap(file.close(h))
        return answer
    }
    return next
}
fn main(){
    let path=unwrap(fs.temp_file(".","hua-native-closure"))
    defer fs.remove(path)
    unwrap(fs.write_text(path,"abc"))
    let next=reader(path)
    print(next())
}
''')
case('n1_clone_hidden_file_capture', '''import std.file as file
import std.fs as fs
fn make() {
    let path=unwrap(fs.temp_file(".","hua-native-closure"))
    defer fs.remove(path)
    let handle=unwrap(file.open(path,"r"))
    defer file.close(handle)
    fn read() int {return 7}
    return read
}
fn main(){
    let f=make()
    print(f())
    let g=clone(f)
    print(g())
}
''')
case('n1_clone_late_file_through_alias', '''import std.file as file
import std.fs as fs
import std.list as list
fn main(){
    let path=unwrap(fs.temp_file(".","hua-native-closure"))
    defer fs.remove(path)
    var empty []file.File=[]
    var handles=list.from_slice(empty)
    fn read() int {return 7}
    let no_file=clone(read)
    print(no_file())
    let handle=unwrap(file.open(path,"r"))
    defer file.close(handle)
    unwrap(list.append(handles,handle))
    clone(read)
}
''')
case('n1_non_callable_checks_after_arguments', '''fn apply(f) {return f(effect())}
fn effect() int {
    print("argument")
    return 3
}
fn main(){apply(4)}
''')
case('n1_clone_file_in_function_slice', """import std.file as file
import std.fs as fs
fn make(path string) {
    let hidden=unwrap(file.open(path,"r"))
    defer file.close(hidden)
    fn get() int { return 3 }
    return get
}
fn main(){
    let path=unwrap(fs.temp_file(".","hua-native-closure"))
    defer fs.remove(path)
    let functions=[make(path)]
    clone(functions)
}
""")
case('n1_dynamic_mut_interface', """interface Counter { mut fn update() int }
struct State { n int }
fn State.update() int {self.n+=1
    return self.n}
fn work(value mut Counter) int {return value.update()}
fn echo(s string) string {return s}
fn select(flag bool) {if flag {return work}
    return echo}
fn main(){
    var state=State{n:8}
    let f=select(true)
    let result int=f(state)
    print(result,state.n)
}
""")
case('n1_higher_order_predicate', """fn decide(f,x) int {
    if f(x) && f(x+1) { return 1 }
    return 0
}
fn positive(x int) bool { return x>0 }
fn main(){ print(decide(positive,4),decide(positive,-2)) }
""")
case('n1_dynamic_return_cleanup', """fn get(f) int {
    defer print("cleanup")
    return f()
}
fn wrong() string { return "not an integer" }
fn main(){ get(wrong) }
""")
case('bound_method_receiver_lifetime', """struct Counter { n int }
fn Counter.bump() int { self.n+=1
    return self.n }
fn Counter.read() int { return self.n }
fn main() {
    var c=Counter{n:4}
    let inc=c.bump
    let read=c.read
    c.n=6
    print(read())
    c=Counter{n:50}
    print(inc(),read(),c.n)
}
""")
case('native_map_removal_and_order', '''import std.maps as maps
fn main() {
    var m=map[int]int{}
    for i in -300..300 { m[i] = i*3 }
    for i in -300..300 {
        if i%3!=0 { print(unwrap(maps.pop(m,i))) }
    }
    print(maps.keys(m))
    for i in -300..300 { m[i] = i*5 }
    for i in -300..300 { print(unwrap(m[i])) }
    for i in -300..300 { delete(m,i) }
    print(len(m),maps.keys(m))
}
''')
case('native_text_preserves_shared_strings', '''import std.strings as strings
fn main() {
    var s = "a"
    let old = s
    var m=map[string]string{s:"original","n": "华"}
    s=s+"b"
    s+=s
    print(s,old,unwrap(m[old]),is_none(m[s]))
    print(strings.last_index("abcabc","bc"),strings.count("aaaaa","aa"),strings.count("华ab",""))
    print(unwrap(strings.template("{{{n:>3}}}",m)),is_err(strings.template("{missing}",m)))
    print(strings.lower_ascii("ÄA"),strings.upper_ascii("aÄ"),strings.trim("  ok\\t"))
}
''')
case('shared_container_capture', '''import std.list as list
fn outer() {
    var xs=list.from_slice([4])
    var ys=[6]
    var scores=map[int]int{0:8}
    fn read() int { return unwrap(list.get(xs,0))+ys[0]+unwrap(scores[0]) }
    unwrap(list.set(xs,0,5))
    ys[0]=7
    scores[0]=9
    xs=list.from_slice([10])
    ys=[11]
    scores=map[int]int{0:12}
    return read
}
fn main() {
    let f=outer()
    print(f())
}
''')
case('list_callback_reentry', '''import std.list as list
var items = list.from_slice([4,3,2,1])
fn keep(x int) bool {
    if x==3 { unwrap(list.append(items,9)) }
    return x>2
}
fn main() { print(list.retain(items,keep)) }
''')
case('file_void_result', '''import std.file as file
import std.fs as fs
fn main() {
    let path=unwrap(fs.temp_file(".","hua-native-file"))
    let first=unwrap(file.open(path,"w"))
    let alias=first
    print(file.close(alias),file.state(first))
    unwrap(fs.remove(path))
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
case('native_task_single_worker', '''import std.task as task
fn wait_inner(child Task<int>) int { return await child }
fn wait_outer(child Task<int>) int { return wait_inner(child) }
async fn leaf(value int, delay int, worker string) int {
    task.sleep(delay)
    task.yield()
    print("leaf", value, task.worker_id() == worker)
    return value * 2
}
async fn parent() int {
    let worker = task.worker_id()
    let first = leaf(21, 1, worker)
    let second = leaf(7, 0, worker)
    print("second", wait_outer(second))
    print("first", wait_outer(first), wait_outer(first))
    return 9
}
fn main() {
    print("root", await parent())
}
''')
case('native_task_scalar_snapshot', '''import std.task as task
fn bridge(value string) string { return str(value) + "!" }
fn worker(value string, amount f64, enabled bool) string {
    task.yield()
    if enabled { print(amount)
        return bridge(value) }
    return "unused"
}
fn main() {
    var label = "before"
    let child = spawn worker(label, f64(1.5), true)
    label = "after"
    print(bridge(label), await child, await child)
}
''')
case('native_task_async_entry_void', '''import std.task as task
async fn report() { task.sleep(1)
    print("child")
}
async fn main() {
    let child = report()
    await child
    print("entry")
}
''')
case('native_task_completion_registration', '''import std.task as task
async fn fast(n int) int { return n + 1 }
async fn parent() {
    for i in 0..80 {
        let child = fast(i)
        if i % 2 == 0 { task.yield() }
        print(await child, await child)
    }
}
fn main() { await parent() }
''')
case('native_task_failed_trace', '''import std.task as task
fn fail_inside(zero int) int { return 10 // zero }
async fn worker(zero int) int { task.sleep(1)
    return fail_inside(zero)
}
fn wait_inside(child Task<int>) int { return await child }
fn main() { print(wait_inside(worker(0))) }
''')
reject('native_task_container', '''fn work(items []int) int { return len(items) }
fn main() {
    let t = spawn work([1, 2])
    print(await t)
}
''')
reject('native_task_global', '''let value = 42
async fn work() int { return value }
fn main() { print(await work()) }
''')
reject('native_task_defer', '''fn done() { print("done") }
async fn work() {
    defer done()
}
fn main() { await work() }
''')
reject('native_task_blocking', '''import std.file as file
async fn work() {
    let handle = file.open("must-not-create", "w")
    print(handle)
}
fn main() { await work() }
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

case('exact_types', '''const fixed i16 = 17
const fraction f32 = 0.25
fn sum(a i16,b i16) i16 { return a+b }
fn main() {
    var a i16 = 100
    var b i16 = 25
    a += b
    print(sum(a,2), a, -a, a>b, type(a), sizeof(a), fixed, fraction)
    let high = u64("18446744073709551615")
    let small = f32(1.25)
    print(high,high >> 60,small+f32(2.5),type(small),alignof(small))
}
''')
case('exact_arithmetic_overflow', '''fn main() {
    var a=u8(255)
    var b=u8(1)
    print(a+b)
}
''')
case('exact_argument_overflow', '''fn widen(value u8) int { return int(value) }
fn next() int {
    print("evaluated")
    return 0
}
fn use(value u8, ignored int) int { return widen(value) }
fn main() { print(use(256,next())) }
''')
case('exact_return_conversion_after_cleanup', '''fn bad() u8 {
    defer print("cleanup")
    return 256
}
fn main() { print(bad()) }
''')
case('fixed_return_conversion_after_cleanup', '''fn bad() [2]int {
    defer print("cleanup")
    let a=[1]
    return a
}
fn main() { print(bad()) }
''')
case('fixed_array_alias_and_borrow', '''fn bump(a mut [3]u16) { a[1] = 7 }
fn take(a [3]u16) int { return int(a[1]) }
fn main() {
    var a [3]u16=[1,2,65535]
    var copied=a
    var slice=a[1:]
    bump(a)
    copied[0]=9
    print(a,copied,slice,take(a),sizeof(a),alignof(a))
    var grid [2][2]i16=[[1,2],[3,4]]
    var row=grid[1][:]
    row[0]=8
    print(grid,grid[1][0],sizeof(grid),alignof(grid))
    var zero [0]u8=[]
    print(len(zero), sizeof(zero), alignof(zero))
}
''')
case('fixed_argument_snapshot_order', '''fn mutate(a mut []u16) int {
    a[0]=9
    return 7
}
fn hear(a [2]u16, marker int) { print(a,marker) }
fn give(a mut [2]u16) ([2]u16,int) { return a,mutate(a) }
fn main() {
    var source [2]u16=[1,2]
    hear(source,mutate(source))
    source[0]=4
    let saved, marker=give(source)
    source[0]=6
    print(saved,marker,source)
}
''')
case('array_literal_conversion_overflow', '''fn main() {
    var a [2]u8=[1,256]
    print(a)
}
''')
case('fixed_array_dynamic_length_error', '''fn make() []int { return [1] }
fn main() {
    var items [2]int=make()
    print(items)
}
''')
case('interface_concrete_dispatch', '''interface Named { fn name() string }
interface Labeled {
    fn name() string
    fn number() int
}
struct First { text string }
struct Second { text string }
fn First.name() string { return "first:"+self.text }
fn First.number() int { return 1 }
fn Second.name() string { return "second:"+self.text }
fn Second.number() int { return 2 }
fn describe(value Named) string { return value.name() }
fn select(first bool) Labeled {
    if first { return First{text:"A"} }
    return Second{text:"B"}
}
fn main() {
    let a=select(true)
    let b=select(false)
    print(describe(a),describe(b),a.number(),b.number(),type(a),type(b))
}
''')
case('mutable_interface_receiver', '''interface Tick { mut fn update() int }
struct Counter { n int }
fn Counter.update() int {
    self.n+=1
    return self.n
}
fn tick(c mut Tick) int { return c.update() }
fn main() {
    var c=Counter{n:2}
    print(tick(c),c.n)
    var v Tick=c
    print(tick(v),c.n)
    print(tick(v),v)
}
''')

case("dynamic_interface_conversion_both_directions", '''interface Name { fn name() string }
interface Wide {
    fn name() string
    fn count() int
}
struct Box { n int }
fn Box.name() string { return "box" }
fn Box.count() int { return self.n }
fn wide() Wide { return Box{n:7} }
fn narrow() Name { return Box{n:7} }
fn as_name(x) Name { return x }
fn as_wide(x) Wide { return x }
fn main() { print(as_name(wide()).name(),as_wide(narrow()).count()) }
''')

reject("dynamic_field_read", '''struct Box { n int }
fn use(x) int { return x.n }
fn main() { print(use(Box{n:7})) }
''')

reject("dynamic_field_write_local", '''struct Box { n int }
fn id(x) { return x }
fn main() {
    var b=id(Box{n:7})
    b.n=9
    print(b.n)
}
''')

reject("dynamic_method_call", '''struct Box { n int }
fn Box.get() int { return self.n }
fn use(x) int { return x.get() }
fn main() { print(use(Box{n:7})) }
''')

reject("dynamic_sequence_index", '''fn use(x) int { return x[1] }
fn main() { print(use([2,3])) }
''')

reject("dynamic_sequence_slice", '''fn use(x) { print(x[1:]) }
fn main() { use([2,3]) }
''')

reject("dynamic_sequence_for", '''fn use(x) {
    for v in x { print(v) }
}
fn main() { use([2,3]) }
''')

reject("dynamic_map_index", '''fn use(x) int { return x["a"] }
fn main() { print(use(map[string]int{"a":7})) }
''')

reject("dynamic_optional_unwrap", '''fn get() int? { return 7 }
fn use(x) int { return unwrap(x) }
fn main() { print(use(get())) }
''')

reject("dynamic_result_test", '''fn get() Result<int,string> { return ok(7) }
fn use(x) bool { return is_ok(x) }
fn main() { print(use(get())) }
''')

reject("dynamic_result_unwrap", '''fn get() Result<int,string> { return ok(7) }
fn use(x) int { return unwrap(x) }
fn main() { print(use(get())) }
''')

reject("dynamic_list_size", '''import std.list as list
fn use(x) int { return list.len(x) }
fn main() { print(use(list.from_slice([2,3]))) }
''')

case('dynamic_interface_concrete_mismatch', """interface Name { fn name() string }
interface Count { fn count() int }
struct Box { n int }
fn Box.name() string { return "box" }
fn only_name() Name { return Box{n:7} }
fn as_count(x) Count { return x }
fn main() { print(as_count(only_name())) }
""")

case('dynamic_interface_mut_preserves_receiver', '''interface Tick { mut fn update() int }
interface ReadWrite {
    mut fn update() int
    fn read() int
}
struct Counter { n int }
fn Counter.update() int { self.n+=1
    return self.n
}
fn Counter.read() int { return self.n }
fn bump(x mut Tick) int { return x.update() }
fn alternate(x mut ReadWrite) bool { return x.update()==0 }
fn select(first bool) {
    if first { return bump }
    return alternate
}
fn main() {
    var c ReadWrite=Counter{n:7}
    let f=select(true)
    print(f(c),c.read())
}
''')

def run(command):
    p = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=30,
                       env=dict(os.environ, HUA_TASK_WORKERS="1"))
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
    if code == 2 and 'native: unsupported' in message and not (work / (name + '.cpp')).exists():
        return name, None
    return name, 'expected rejection without C++ output, got %d: %s' % (code, message[-600:])
failures = []
with concurrent.futures.ThreadPoolExecutor(6) as pool:
    for name, problem in list(pool.map(check, cases.items())) + list(pool.map(refuse, rejected.items())):
        if problem: failures.append((name, problem))
for name, problem in failures: print('FAIL', name, '\n', problem)
if failures: sys.exit(1)
shutil.rmtree(work, ignore_errors=True)
print(f'native backend differential OK ({len(cases)} programs identical to the VM, {len(rejected)} rejected)')
