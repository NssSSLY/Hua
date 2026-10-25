# HUA-D038 P1: alg.partition/zip_with/windows and list.dedup (since 20). Every case
# runs on the AST interpreter, the source VM and a source-free HUAB.
from pathlib import Path
import struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0
def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]

with tempfile.TemporaryDirectory(prefix='hua-alg-p1-',dir=build) as temp:
    base=Path(temp);n=0
    def source(text):
        global n
        n+=1;p=base/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def run(args,code=0):
        global checks
        r=subprocess.run([str(exe),*map(str,args)],cwd=base,capture_output=True,timeout=120)
        out=r.stdout.decode('utf-8').replace('\r\n','\n');err=r.stderr.decode('utf-8').replace('\r\n','\n')
        assert r.returncode==code,(args,r.returncode,out,err);checks+=1;return out,err
    def three(text,code=0):
        """AST interpreter, source VM and source-free HUAB; returns the (stdout, stderr) pairs."""
        p=source(text);artifact=base/f'case{n}.huab';run(['build',p,'-o',artifact])
        results=[run(['interpret',p],code),run(['run',p],code)]
        hidden=p.with_suffix('.moved');p.rename(hidden);results.append(run(['run',artifact],code));hidden.rename(p)
        return results,artifact
    def same(text,want):
        results,artifact=three(text)
        assert all(o==want for o,_ in results),(want,results);return artifact

    # partition: stable halves, all/none, empty, structs, one call per element left to right.
    artifact=same('''import std.alg as alg
struct P { x int }
var trace = ""
fn even(x int) bool {
    trace = trace + str(x)
    return x % 2 == 0
}
fn yes(x int) bool { return true }
fn big(p P) bool { return p.x > 1 }
fn main() {
    let a, b = alg.partition([5, 2, 3, 8, 4, 1], even)
    print(a, b, trace)
    let c, d = alg.partition([1, 2], yes)
    print(c, d, len(d))
    let e []int = []
    let f, g = alg.partition(e, even)
    print(f, g)
    let arr [3]P = [P{x: 1}, P{x: 2}, P{x: 3}]
    let h, i = alg.partition(arr, big)
    print(h, i)
    var w = clone(h)
    w[0].x = 9
    print(w, h)
}
''','[2, 8, 4] [5, 3, 1] 523841\n[1, 2] [] 0\n[] []\n[P{x: 2}, P{x: 3}] [P{x: 1}]\n[P{x: 9}, P{x: 3}] [P{x: 2}, P{x: 3}]\n')
    assert header_format(artifact)==20,header_format(artifact)
    # zip_with: shorter input wins either way, empty, differing element and result types.
    same('''import std.alg as alg
fn add(x int, y int) int { return x + y }
fn label(x int, s string) string { return s + str(x) }
fn half(x float, y int) bool { return x * 2.0 > float(y) }
fn main() {
    print(alg.zip_with([1, 2, 3], [10, 20], add), alg.zip_with([1], [10, 20, 30], add))
    print(alg.zip_with([1, 2], ["a", "b"], label))
    let e []int = []
    print(alg.zip_with(e, [1, 2], add), len(alg.zip_with([1, 2], e, add)))
    print(alg.zip_with([1.5, 0.5], [2, 2], half))
}
''','[11, 22] [11]\n[a1, b2]\n[] 0\n[true, false]\n')
    # windows: sizes 1, n and > n, empty, struct and nested elements, read-only views of one copy.
    same('''import std.alg as alg
struct P { x int }
fn main() {
    print(alg.windows([1, 2, 3], 1), alg.windows([1, 2, 3], 3), alg.windows([1, 2, 3], 4))
    let e []string = []
    print(alg.windows(e, 2), len(alg.windows(e, 1)))
    print(alg.windows(["a", "b", "c", "d"], 2))
    print(alg.windows([P{x: 1}, P{x: 2}, P{x: 3}], 2))
    let nested [][]int = [[1], [2, 3], [4]]
    let ws = alg.windows(nested, 2)
    print(ws, ws[1][0][1])
    var s = clone(alg.unique([1, 2, 3]))
    let v = alg.windows(s, 2)
    s[0] = 9
    print(v, s)
    var copy = clone(v)
    copy[0][0] = 7
    print(copy, v)
    let arr [4]float = [0.5, 1.5, 2.5, 3.5]
    print(alg.windows(arr, 3))
}
''','[[1], [2], [3]] [[1, 2, 3]] []\n[] 0\n[[a, b], [b, c], [c, d]]\n[[P{x: 1}, P{x: 2}], [P{x: 2}, P{x: 3}]]\n[[[1], [2, 3]], [[2, 3], [4]]] 3\n[[1, 2], [2, 3]] [9, 2, 3]\n[[7, 2], [2, 3]] [[1, 2], [2, 3]]\n[[0.5, 1.5, 2.5], [1.5, 2.5, 3.5]]\n')
    # list.dedup: adjacent runs only, +-0 equal, strings, structs, empty, the removed count.
    same('''import std.list as list
struct P { x int }
fn main() {
    var a = list.from_slice([1, 1, 2, 2, 2, 1, 3, 3])
    print(list.dedup(a), list.snapshot(a), list.len(a))
    var b = list.from_slice([1, 2, 3])
    print(list.dedup(b), list.snapshot(b))
    var c = list.from_slice([4, 4, 4])
    print(list.dedup(c), list.snapshot(c))
    var d = list.from_slice([0.0, -0.0, 1.5, 1.5, -0.0])
    print(list.dedup(d), list.snapshot(d))
    var e = list.from_slice(["x", "x", "", "", "y", "x"])
    print(list.dedup(e), list.snapshot(e))
    var f = list.from_slice([P{x: 1}, P{x: 1}, P{x: 2}])
    print(list.dedup(f), list.snapshot(f))
    let none []int = []
    var g = list.from_slice(none)
    print(list.dedup(g), list.len(g))
    print(list.dedup(a), list.snapshot(a))
}
''','4 [1, 2, 1, 3] 4\n0 [1, 2, 3]\n2 [4]\n2 [0, 1.5, -0]\n2 [x, , y, x]\n1 [P{x: 1}, P{x: 2}]\n0 0\n0 [1, 2, 1, 3]\n')
    # Runtime errors on three paths.
    for body,error in [('print(alg.windows([1, 2], 0))','E4003'),('print(alg.windows([1, 2], -1))','E4003'),
                       ('let s = strings.repeat("x", 9000000)\n    print(len(alg.windows([s, s + "y"], 1)))','E4099')]:
        results,_=three('import std.alg as alg\nimport std.strings as strings\nfn main() {\n    '+body+'\n}\n',1)
        assert all(error in e for _,e in results),(body,results)
    # Static errors.
    for body,error in [('fn f(x int) int { return x }\nfn main() { let a, b = alg.partition([1], f) }','E3004'),
                       ('fn f(x string) bool { return true }\nfn main() { let a, b = alg.partition([1], f) }','E3004'),
                       ('fn f(x int) bool { return true }\nfn main() { let a = alg.partition([1], f) }','E3006'),
                       ('fn f(x int) bool { return true }\nfn main() { let a, b, c = alg.partition([1], f) }','E3006'),
                       ('fn f(x int) bool { return true }\nfn main() {\n    let a, b = alg.partition([1], f)\n    a[0] = 2\n}','E3003'),
                       ('fn f(x int) int { return x }\nfn main() { print(alg.zip_with([1], [2], f)) }','E3005'),
                       ('fn f(x int, y int) int { return x }\nfn main() { print(alg.zip_with([1], ["a"], f)) }','E3004'),
                       ('fn f(x int, y int) int { return x }\nfn main() { print(alg.zip_with([1], 2, f)) }','E3004'),
                       ('fn main() { print(alg.windows([1], "2")) }','E3004'),
                       ('fn main() { print(alg.windows(1, 2)) }','E3004'),
                       ('fn main() {\n    let w = alg.windows([1, 2], 1)\n    w[0][0] = 5\n}','E3003'),
                       ('import std.list as list\nfn main() {\n    let l = list.from_slice([1, 1])\n    print(list.dedup(l))\n}','E3003'),
                       ('import std.list as list\nfn main() { print(list.dedup([1, 1])) }','E3004')]:
        p=source('import std.alg as alg\n'+body+'\n');_,e=run(['check',p],1);assert error in e,(body,e)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,19);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    _,e=run(['run',bad],1);assert 'E6002' in e,e
print(f'alg P1 checks passed: {checks}')
