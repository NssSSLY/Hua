# HUA-D038 slice 2 (std.maps, std.sets, list P0) across three execution paths.
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

with tempfile.TemporaryDirectory(prefix='hua-collections-',dir=build) as temp:
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
        p=fixture('import std.maps as maps\nimport std.sets as sets\nimport std.list as list\nfn main() {\n'+body+'\n}\n')
        run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:run(command,target,cwd=work,code=1,error=error)

    artifact=triple('''import std.maps as maps
import std.sets as sets
import std.list as list
struct Point { x int }
fn main() {
    var m = map[string]int{"pear": 3, "apple": 1, "fig": 2}
    let ks = maps.keys(m)
    print(ks, maps.values(m))
    let names, counts = maps.entries(m)
    m["banana"] = 9
    print(names, counts, ks, maps.keys(m))
    var empty = map[int]bool{}
    let ek, ev = maps.entries(empty)
    print(maps.keys(empty), maps.values(empty), len(ek), len(ev))
    var flags = map[bool]string{true: "t", false: "f"}
    print(maps.keys(flags), maps.values(flags))
    print(maps.get_or(m, "fig", 0), maps.get_or(m, "kiwi", -1), len(m))
    var maybe = map[string]int?{"a": nil, "b": 2}
    print(maps.get_or(maybe, "a", 7), maps.get_or(maybe, "z", 7), maps.pop(maybe, "a"), has(maybe, "a"), maps.pop(maybe, "z"))
    print(maps.pop(m, "pear"), maps.pop(m, "pear"), maps.keys(m))
    var more = map[string]int{"apple": 10, "cherry": 4, "zz": 5}
    print(maps.merge(m, more), m, maps.merge(m, more), maps.merge(m, m), len(more))
    var big = map[int]int{}
    for i in 0..10 { big[i * 2] = i }
    var odd = map[int]int{}
    for i in 0..10 { odd[i * 2 + 1] = i }
    print(maps.merge(big, odd), len(big), maps.keys(big))
    let a = sets.from_slice([5, 1, 3, 1, 5])
    let b = sets.from_slice([3, 4])
    let none = sets.from_slice(empty_ints())
    print(a, b, none)
    print(sets.union(a, b), sets.intersection(a, b), sets.difference(a, b), sets.difference(b, a))
    print(sets.union(a, none), sets.intersection(a, none), sets.is_subset(none, a), sets.is_subset(a, a), sets.is_subset(a, b))
    print(sets.is_disjoint(a, b), sets.is_disjoint(a, none), sets.is_disjoint(sets.from_slice([2]), a))
    let words = sets.from_slice(["b", "a", "b"])
    var labels = map[string]string{"a": "x", "c": "y"}
    print(words, sets.intersection(words, labels), sets.is_subset(sets.from_slice(["a"]), labels))
    var u = sets.union(a, b)
    u[9] = true
    print(len(u), sets.from_slice([true, false, true]))
    var e []int = []
    var l = list.from_slice(e)
    print(unwrap(list.insert(l, 0, 4)), unwrap(list.insert(l, 0, 2)), unwrap(list.insert(l, 2, 8)), list.snapshot(l), list.insert(l, 4, 1), list.insert(l, -1, 1))
    print(unwrap(list.remove_at(l, 2)), list.remove_at(l, 2), list.snapshot(l))
    print(list.index_of(l, 4), list.index_of(l, 5), list.contains(l, 2), list.contains(l, 7))
    var pts = list.from_slice([Point{x: 1}, Point{x: 2}])
    print(list.index_of(pts, Point{x: 2}), list.contains(pts, Point{x: 3}))
    var nums = list.from_slice([3, -1, 2, 10, 2])
    list.sort(nums)
    print(list.snapshot(nums))
    list.reverse(nums)
    print(list.snapshot(nums))
    var fs = list.from_slice([2.5, -0.5, 1.0])
    list.sort(fs)
    var ss = list.from_slice(["b", "", "ab", "a"])
    list.sort(ss)
    print(list.snapshot(fs), list.snapshot(ss))
    var none_l = list.from_slice(e)
    list.sort(none_l)
    list.reverse(none_l)
    print(list.snapshot(none_l), list.slice(none_l, 0, 0), list.swap(none_l, 0, 0))
    print(unwrap(list.swap(nums, 0, 4)), unwrap(list.swap(nums, 1, 1)), list.snapshot(nums), list.swap(nums, 0, 5))
    print(unwrap(list.slice(nums, 1, 3)), unwrap(list.slice(nums, 5, 5)), list.slice(nums, 3, 2), list.slice(nums, 0, 6))
    list.truncate(nums, 9)
    print(list.len(nums))
    list.truncate(nums, 2)
    print(list.snapshot(nums))
    list.truncate(nums, 0)
    print(list.len(nums))
}
fn empty_ints() []int {
    var e []int = []
    return e
}
''','''[apple, fig, pear] [1, 2, 3]
[apple, fig, pear] [1, 2, 3] [apple, fig, pear] [apple, banana, fig, pear]
[] [] 0 0
[false, true] [f, t]
2 -1 4
nil 7 nil false nil
3 nil [apple, banana, fig]
2 map{apple: 10, banana: 9, cherry: 4, fig: 2, zz: 5} 0 0 3
10 20 [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19]
map{1: true, 3: true, 5: true} map{3: true, 4: true} map{}
map{1: true, 3: true, 4: true, 5: true} map{3: true} map{1: true, 5: true} map{4: true}
map{1: true, 3: true, 5: true} map{} true true false
false true true
map{a: true, b: true} map{a: true} true
5 map{false: true, true: true}
1 2 3 [2, 4, 8] err(list index out of bounds) err(list index out of bounds)
8 err(list index out of bounds) [2, 4]
1 -1 true false
1 false
[-1, 2, 2, 3, 10]
[10, 3, 2, 2, -1]
[-0.5, 1, 2.5] [, a, ab, b]
[] ok([]) err(list index out of bounds)
true true [-1, 3, 2, 2, 10] err(list index out of bounds)
[3, 2] [] err(list slice out of bounds) err(list slice out of bounds)
5
[-1, 3]
0
''')
    assert header_format(artifact)==10,header_format(artifact)
    # Programs that use none of the new functions keep their previous format.
    old=triple('import std.list as list\nfn main() { print(list.len(list.from_slice([1]))) }\n','1\n')
    assert header_format(old)<10,header_format(old)
    # A format-10 program whose header claims format 9 is rejected before execution.
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,9);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    run('run',bad,code=1,error='E6002')
    # Programmer errors are runtime diagnostics on every path.
    runtime_failure('    var l = list.from_slice([1])\n    list.truncate(l, -1)','E4003')
    # Static signature checks (E3004).
    for body in ['var a = map[int]bool{}\n    var b = map[string]bool{}\n    print(sets.union(a, b))',
                 'var l = list.from_slice([true])\n    list.sort(l)',
                 'print(maps.keys(5))',
                 'var m = map[string]int{}\n    print(maps.get_or(m, 1, 2))',
                 'var m = map[string]int{}\n    print(maps.merge(m, map[string]string{}))',
                 'var l = list.from_slice([1])\n    print(list.index_of(l, "x"))',
                 'print(sets.from_slice([1.5]))']:
        p=fixture('import std.maps as maps\nimport std.sets as sets\nimport std.list as list\nfn main() {\n    '+body+'\n}\n')
        run('check',p,code=1,error='E3004')
    # The mutating functions need a writable map/List, as delete and the existing List writers.
    for param,call in [('m map[string]int','maps.pop(m, "a")'),('m map[string]int','maps.merge(m, map[string]int{})'),
                       ('l List<int>','unwrap(list.insert(l, 0, 1))'),('l List<int>','unwrap(list.remove_at(l, 0))'),('l List<int>','list.reverse(l)'),
                       ('l List<int>','list.sort(l)'),('l List<int>','unwrap(list.swap(l, 0, 0))'),('l List<int>','list.truncate(l, 0)')]:
        p=fixture('import std.maps as maps\nimport std.list as list\nfn take('+param+') { '+call+' }\nfn main() { }\n')
        run('check',p,code=1,error='E3003')
print(f'collections P0 three-path checks passed: {checks}')
