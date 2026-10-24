# HUA-D038 P0 gap: alg.unique/chunk/product (since 19). Every case runs on the AST
# interpreter, the source VM and a source-free HUAB.
from pathlib import Path
import struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0
def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]

with tempfile.TemporaryDirectory(prefix='hua-alg-p0-',dir=build) as temp:
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

    # unique: first occurrence order, int/float/string, -0.0 == 0.0 keeps the first, arrays.
    artifact=same('''import std.alg as alg
fn main() {
    print(alg.unique([3, 1, 3, 2, 1]))
    print(alg.unique([5, 5, 5]), alg.unique([1, 2, 3]))
    print(alg.unique([-0.0, 1.5, 0.0, 1.5]), alg.unique([0.0, -0.0]))
    print(alg.unique(["b", "a", "b", "", ""]))
    let e []string = []
    print(alg.unique(e), len(alg.unique(e)))
    let arr [5]int = [4, 4, 2, 4, 2]
    print(alg.unique(arr))
    var c = clone(alg.unique([1]))
    c[0] = 2
    print(c)
}
''','[3, 1, 2]\n[5] [1, 2, 3]\n[-0, 1.5] [0]\n[b, a, ]\n[] 0\n[4, 2]\n[2]\n')
    assert header_format(artifact)==19,header_format(artifact)
    # unique over many values (hash table growth) matches a reference.
    same('''import std.alg as alg
import std.list as list
fn main() {
    var empty []int = []
    var l = list.from_slice(empty)
    for i in 0..30000 { unwrap(list.append(l, (i * 7919) % 12345 - 6000)) }
    let u = alg.unique(list.snapshot(l))
    print(len(u), u[0], u[1], u[len(u) - 1])
}
''',f'{len(dict.fromkeys((i*7919)%12345-6000 for i in range(30000)))} {[*dict.fromkeys((i*7919)%12345-6000 for i in range(30000))][0]} {[*dict.fromkeys((i*7919)%12345-6000 for i in range(30000))][1]} {[*dict.fromkeys((i*7919)%12345-6000 for i in range(30000))][-1]}\n')
    # chunk: even and uneven splits, size > n, size 1, empty, struct and nested elements.
    same('''import std.alg as alg
struct P { x int }
fn main() {
    print(alg.chunk([1, 2, 3, 4], 2), alg.chunk([1, 2, 3, 4, 5], 2))
    print(alg.chunk(["a", "b"], 5), alg.chunk([7, 8, 9], 1))
    let e []int = []
    print(alg.chunk(e, 3), len(alg.chunk(e, 3)))
    print(alg.chunk([P{x: 1}, P{x: 2}, P{x: 3}], 2))
    let nested [][]int = [[1], [2, 3], [4]]
    let c = alg.chunk(nested, 2)
    print(c, len(c[0]), c[0][1][1])
    let arr [3]float = [1.5, 2.5, 3.5]
    print(alg.chunk(arr, 2))
    var copy = clone(alg.chunk([1, 2, 3], 2))
    copy[1][0] = 9
    print(copy)
}
''','[[1, 2], [3, 4]] [[1, 2], [3, 4], [5]]\n[[a, b]] [[7], [8], [9]]\n[] 0\n[[P{x: 1}, P{x: 2}], [P{x: 3}]]\n[[[1], [2, 3]], [[4]]] 2 3\n[[1.5, 2.5], [3.5]]\n[[1, 2], [9]]\n')
    # product: int/float, empty 1/1.0, zero, overflow at either bound, float overflow.
    same('''import std.alg as alg
fn main() {
    print(unwrap(alg.product([2, 3, 7])), unwrap(alg.product([-2, 5])), unwrap(alg.product([4, 0, 9])))
    let ei []int = []
    let ef []float = []
    print(unwrap(alg.product(ei)), unwrap(alg.product(ef)), unwrap(alg.product([1.5, 2.0])))
    print(alg.product([9223372036854775807, 2]))
    print(alg.product([-9223372036854775807, 2]))
    print(unwrap(alg.product([-4611686018427387904, 2])))
    print(alg.product([1.0e300, 1.0e300]))
    print(alg.product([4611686018427387904, 4, 0]))
}
''','42 -10 0\n1 1 3\nerr(ALG_OVERFLOW: int64 product overflow)\nerr(ALG_OVERFLOW: int64 product overflow)\n-9223372036854775808\nerr(ALG_OVERFLOW: float product overflow)\nerr(ALG_OVERFLOW: int64 product overflow)\n')
    # Runtime errors on three paths.
    for body,error in [('print(alg.chunk([1, 2], 0))','E4003'),('print(alg.chunk([1, 2], -3))','E4003'),
                       ('let s = strings.repeat("x", 9000000)\n    print(len(alg.unique([s, s + "y"])))','E4099'),
                       ('let s = strings.repeat("x", 9000000)\n    print(len(alg.chunk([s, s + "y"], 1)))','E4099')]:
        results,_=three('import std.alg as alg\nimport std.strings as strings\nfn main() {\n    '+body+'\n}\n',1)
        assert all(error in e for _,e in results),(body,results)
    # Static errors.
    for body,error in [('struct P { x int }\nfn main() { print(alg.unique([P{x: 1}])) }','E3004'),('fn main() { print(alg.unique([true])) }','E3004'),
                       ('fn main() { print(alg.product(["a"])) }','E3004'),('fn main() { print(alg.chunk([1], "2")) }','E3004'),
                       ('fn main() { print(alg.chunk(1, 2)) }','E3004'),
                       ('fn main() {\n    let c = alg.chunk([1, 2], 1)\n    c[0][0] = 5\n}','E3003'),
                       ('fn main() {\n    var u = alg.unique([1, 2])\n    u[0] = 5\n}','E3003'),
                       ('fn main() { let x []int = alg.chunk([1], 1) }','E3004')]:
        p=source('import std.alg as alg\n'+body+'\n');_,e=run(['check',p],1);assert error in e,(body,e)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,18);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    _,e=run(['run',bad],1);assert 'E6002' in e,e
print(f'alg P0 checks passed: {checks}')
