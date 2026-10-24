# HUA-D038 L0-1 (standard functions calling Hua callbacks) across three execution paths.
from pathlib import Path
import difflib,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0

def run(command,path,*extra,cwd=None,expected=None,code=0,error=None):
    global checks
    p=subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=cwd,capture_output=True,timeout=60)
    out=p.stdout.decode('utf-8').replace('\r\n','\n');err=p.stderr.decode('utf-8')
    assert p.returncode==code,(command,path,p.returncode,out,err)
    if expected is not None:assert out==expected,(command,''.join(difflib.unified_diff(expected.splitlines(True),out.splitlines(True),'expected','actual')),err)
    if error:assert error in err,(command,out,err,error)
    checks+=1;return out

def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]

with tempfile.TemporaryDirectory(prefix='hua-callbacks-',dir=build) as temp:
    base=Path(temp);source=base/'source';source.mkdir();work=base/'work';work.mkdir();n=0
    def fixture(text):
        global n
        n+=1;p=source/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def triple(text,expected):
        p=fixture(text);run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.unlink();run('check',artifact,expected='OK\n')
            run(command,target,cwd=work,expected=expected)
        return artifact
    def runtime_failure(body,error):
        p=fixture('import std.alg as alg\nimport std.list as list\nimport std.maps as maps\n'+body)
        run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:run(command,target,cwd=work,code=1,error=error)

    artifact=triple('''import std.alg as alg
import std.list as list
import std.maps as maps
struct Item { id int
    score int }
fn scale(x int) int { return x * 3 + 1 }
fn step(acc int, x int) int { return acc + x * x }
fn even(x int) bool { return x % 2 == 0 }
fn big(x int) bool { return x > 100 }
fn neg(x int) int { return 0 - x }
fn score(it Item) int { return it.score }
fn name(it Item) string { return "k" + str(it.score) }
fn by_score(a Item, b Item) bool { return a.score < b.score }
fn desc(a int, b int) bool { return a > b }
fn half(x float) float { return x / 2.0 }
fn keep_odd(x int) bool { return x % 2 == 1 }
fn inc(n int) int { return n + 1 }
fn main() {
    let xs = [5, 2, 8, 3, 2]
    print(alg.map(xs, scale), alg.fold(xs, 0, step), alg.filter(xs, even), alg.filter(xs, big))
    print(alg.any(xs, even), alg.any(xs, big), alg.all(xs, even), alg.all(alg.filter(xs, even), even))
    print(alg.find(xs, even), alg.find(xs, big), alg.find_index(xs, even), alg.find_index(xs, big))
    print(alg.sorted_by_key(xs, neg), alg.sorted_by(xs, desc), unwrap(alg.min_by_key(xs, neg)), unwrap(alg.max_by_key(xs, neg)))
    var none []int = []
    print(alg.map(none, scale), alg.filter(none, even), alg.any(none, even), alg.all(none, even), alg.find(none, even), alg.find_index(none, even))
    print(alg.sorted_by_key(none, neg), alg.sorted_by(none, desc), alg.min_by_key(none, neg), alg.max_by_key(none, neg))
    let items = [Item{id: 1, score: 30}, Item{id: 2, score: 10}, Item{id: 3, score: 30}, Item{id: 4, score: 20}]
    print(alg.sorted_by_key(items, score), alg.sorted_by(items, by_score))
    print(alg.sorted_by_key(items, name), unwrap(alg.min_by_key(items, score)), unwrap(alg.max_by_key(items, score)))
    print(alg.map([1.0, 3.0], half))
    var l = list.from_slice([4, 1, 3, 1, 5])
    list.sort_by_key(l, neg)
    print(list.snapshot(l))
    list.sort_by(l, desc)
    print(list.snapshot(l))
    var stable = list.from_slice(items)
    list.sort_by_key(stable, score)
    print(list.snapshot(stable))
    print(list.retain(l, keep_odd), list.snapshot(l), list.retain(l, keep_odd))
    var counts = map[string]int{}
    for w in ["a", "b", "a", "c", "a"] { maps.update(counts, w, 0, inc) }
    print(counts, maps.update(counts, "b", 0, inc), counts)
}
''','''[16, 7, 25, 10, 7] 106 [2, 8, 2] []
true false false true
2 nil 1 -1
[8, 5, 3, 2, 2] [8, 5, 3, 2, 2] 8 2
[] [] false true nil -1
[] [] err(ALG_EMPTY: min_by_key requires non-empty input) err(ALG_EMPTY: max_by_key requires non-empty input)
[Item{id: 2, score: 10}, Item{id: 4, score: 20}, Item{id: 1, score: 30}, Item{id: 3, score: 30}] [Item{id: 2, score: 10}, Item{id: 4, score: 20}, Item{id: 1, score: 30}, Item{id: 3, score: 30}]
[Item{id: 2, score: 10}, Item{id: 4, score: 20}, Item{id: 1, score: 30}, Item{id: 3, score: 30}] Item{id: 2, score: 10} Item{id: 1, score: 30}
[0.5, 1.5]
[5, 4, 3, 1, 1]
[5, 4, 3, 1, 1]
[Item{id: 2, score: 10}, Item{id: 4, score: 20}, Item{id: 1, score: 30}, Item{id: 3, score: 30}]
1 [5, 3, 1, 1] 0
map{a: 3, b: 2, c: 1} 2 map{a: 3, b: 2, c: 1}
''')
    assert header_format(artifact)==11,header_format(artifact)
    # Programs that use none of the callback functions keep their previous format.
    old=triple('import std.maps as maps\nfn main() { print(maps.keys(map[int]int{2: 1})) }\n','[2]\n')
    assert header_format(old)<11,header_format(old)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,10);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    run('run',bad,code=1,error='E6002')
    # Callback diagnostics propagate with the standard call site in the trace.
    runtime_failure('fn bad(x int) int { return 10 // (x - 2) }\nfn main() { print(alg.map([1, 2, 3], bad)) }\n','E4004')
    runtime_failure('fn bad(acc int, x int) int { return acc // (x - 2) }\nfn main() { print(alg.fold([1, 2], 5, bad)) }\n','E4004')
    # Callback re-entry counts toward the call depth limit.
    runtime_failure('fn deep(x int) int {\n    if x == 0 { return 0 }\n    return alg.fold([x - 1], 0, add)\n}\nfn add(a int, x int) int { return a + deep(x) }\nfn main() { print(deep(200)) }\n','E4099')
    # E5: a callback that changes the same List (through a module global) stops the call.
    runtime_failure('var g = list.from_slice([3, 1, 2])\nfn key(x int) int {\n    unwrap(list.append(g, 9))\n    return x\n}\nfn main() { list.sort_by_key(g, key) }\n','list modified during callback')
    runtime_failure('var g = list.from_slice([3, 1, 2])\nfn less(a int, b int) bool {\n    list.clear(g)\n    return a < b\n}\nfn main() { list.sort_by(g, less) }\n','list modified during callback')
    runtime_failure('var g = list.from_slice([3, 1, 2])\nfn keep(x int) bool {\n    unwrap(list.set(g, 0, 7))\n    return true\n}\nfn main() { print(list.retain(g, keep)) }\n','list modified during callback')
    # Static callback checks.
    for body,code in [('fn f(x string) int { return 1 }\nfn main() { print(alg.map([1, 2], f)) }','E3004'),
                      ('fn f(x int, y int) int { return 1 }\nfn main() { print(alg.map([1, 2], f)) }','E3005'),
                      ('fn f(x) int { return 1 }\nfn main() { print(alg.map([1, 2], f)) }','E3004'),
                      ('fn f(mut x int) int { return 1 }\nfn main() { print(alg.map([1, 2], f)) }','E3004'),
                      ('fn f(x int) { print(x) }\nfn main() { print(alg.map([1, 2], f)) }','E3004'),
                      ('fn f(acc string, x int) int { return 1 }\nfn main() { print(alg.fold([1, 2], "", f)) }','E3004'),
                      ('fn f(x int) int { return x }\nfn main() { print(alg.filter([1, 2], f)) }','E3004'),
                      ('fn f(x int) bool { return true }\nfn main() { print(alg.sorted_by_key([1, 2], f)) }','E3004'),
                      ('fn f(a int) bool { return true }\nfn main() { print(alg.sorted_by([1, 2], f)) }','E3005'),
                      ('fn f(x int) string { return "" }\nfn main() {\n    var m = map[string]int{}\n    print(maps.update(m, "a", 0, f))\n}','E3004'),
                      ('fn main() { print(alg.map([1, 2], 3)) }','E3004'),
                      ('fn key(x int) int { return x }\nfn take(l List<int>) { list.sort_by_key(l, key) }\nfn main() { }','E3003'),
                      ('fn keep(x int) bool { return true }\nfn take(l List<int>) { print(list.retain(l, keep)) }\nfn main() { }','E3003'),
                      ('fn inc(x int) int { return x }\nfn take(m map[string]int) { print(maps.update(m, "a", 0, inc)) }\nfn main() { }','E3003')]:
        p=fixture('import std.alg as alg\nimport std.list as list\nimport std.maps as maps\n'+body+'\n')
        run('check',p,code=1,error=code)
print(f'L0-1 callback three-path checks passed: {checks}')
