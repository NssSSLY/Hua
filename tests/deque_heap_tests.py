# HUA-D038 step 5 group 1 (std.deque, std.heap) across three execution paths.
# Random operation sequences are checked against Python's deque and heapq.
from pathlib import Path
from collections import deque
import difflib,heapq,random,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0

def run(command,path,*extra,cwd=None,expected=None,code=0,error=None):
    global checks
    p=subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=cwd,capture_output=True,timeout=120)
    out=p.stdout.decode('utf-8').replace('\r\n','\n');err=p.stderr.decode('utf-8')
    assert p.returncode==code,(command,path,p.returncode,out,err)
    if expected is not None:assert out==expected,(command,''.join(difflib.unified_diff(expected.splitlines(True),out.splitlines(True),'expected','actual')),err)
    if error:assert error in err,(command,out,err,error)
    checks+=1;return out
def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]
fmt=lambda xs:'['+', '.join(map(str,xs))+']'

rng=random.Random(20261024)
program=['import std.deque as deque','import std.heap as heap','import std.list as list',
         'struct Job { pri int','    id int }','fn pri(j Job) int { return j.pri }','fn neg(j Job) int { return 0 - j.pri }','fn main() {','    var e []int = []']
expected=[]
# Deque: 400 random operations at both ends, printed after every operation.
model=deque();program.append('    var d = deque.from_slice(e)')
for step in range(400):
    op=rng.choice(['pb','pf','ob','of','f','b','g'] if model else ['pb','pf','ob','of'])
    if op=='pb':v=rng.randrange(-50,50);model.append(v);program.append(f'    print(unwrap(deque.push_back(d, {v})))');expected.append(str(len(model)))
    elif op=='pf':v=rng.randrange(-50,50);model.appendleft(v);program.append(f'    print(unwrap(deque.push_front(d, {v})))');expected.append(str(len(model)))
    elif op in('ob','of'):
        name='pop_back' if op=='ob' else 'pop_front'
        if not model:program.append(f'    print(deque.{name}(d))');expected.append('err(DEQUE_EMPTY: deque is empty)')
        else:v=model.pop() if op=='ob' else model.popleft();program.append(f'    print(unwrap(deque.{name}(d)))');expected.append(str(v))
    elif op=='f':program.append('    print(unwrap(deque.front(d)), unwrap(deque.back(d)), deque.len(d))');expected.append(f'{model[0]} {model[-1]} {len(model)}')
    else:i=rng.randrange(len(model));program.append(f'    print(unwrap(deque.get(d, {i})))');expected.append(str(model[i]))
program.append('    print(deque.snapshot(d))');expected.append(fmt(list(model)))
# Heap: 600 random push/pop operations (pop order is fully determined).
h=[];program.append('    var h = list.from_slice(e)')
for step in range(600):
    if h and rng.random()<0.45:program.append('    print(unwrap(heap.pop(h)))');expected.append(str(heapq.heappop(h)))
    else:v=rng.randrange(-1000,1000);heapq.heappush(h,v);program.append(f'    print(unwrap(heap.push(h, {v})))');expected.append(str(len(h)))
program.append('    while list.len(h) > 0 { print(unwrap(heap.pop(h))) }');expected+= [str(heapq.heappop(h)) for _ in range(len(h))]
# heapify of arbitrary input, then drain.
xs=[rng.randrange(0,100) for _ in range(57)]
program+=[f'    var raw = list.from_slice({fmt(xs)})','    heap.heapify(raw)','    while list.len(raw) > 0 { print(unwrap(heap.pop(raw))) }'];expected+= list(map(str,sorted(xs)))
# top-k and floats/strings.
ys=[rng.randrange(0,10000) for _ in range(300)]
program.append(f'    let ys = {fmt(ys)}');program.append('    print(unwrap(heap.smallest(ys, 10)), unwrap(heap.largest(ys, 10)), len(unwrap(heap.smallest(ys, 0))), len(unwrap(heap.largest(ys, 999))))')
expected.append(f'{fmt(sorted(ys)[:10])} {fmt(sorted(ys,reverse=True)[:10])} 0 300')
program+=['    var fs = list.from_slice([2.5, -1.0, 0.5])','    heap.heapify(fs)','    var ss = list.from_slice(["pear", "apple", "fig", "applf"])','    heap.heapify(ss)',
          '    print(unwrap(heap.pop(fs)), unwrap(heap.pop(fs)), unwrap(heap.pop(ss)), unwrap(heap.pop(ss)), unwrap(heap.peek(ss)))']
expected.append('-1 0.5 apple applf fig')
# By-key heap with struct payloads (unique keys, so payload order is determined).
keys=rng.sample(range(10000),200);none_list='    var none []Job = []'
program+=[none_list,'    var jobs = list.from_slice(none)']
for i,k in enumerate(keys):program.append(f'    unwrap(heap.push_by_key(jobs, Job{{pri: {k}, id: {i}}}, pri))')
program.append('    while list.len(jobs) > 0 {\n        let j = unwrap(heap.pop_by_key(jobs, pri))\n        print(j.pri, j.id)\n    }')
expected+=[f'{k} {keys.index(k)}' for k in sorted(keys)]
program+=['    var maxes = list.from_slice(none)','    for k in [3, 9, 1, 7] { unwrap(heap.push_by_key(maxes, Job{pri: k, id: k}, neg)) }','    print(unwrap(heap.pop_by_key(maxes, neg)).pri, unwrap(heap.pop_by_key(maxes, neg)).pri, heap.pop_by_key(list.from_slice(none), pri))']
expected.append('9 7 err(HEAP_EMPTY: heap is empty)')
# Deque handle rules: clone is independent, == compares elements, nested reads are read-only copies.
program+=['    var a = deque.from_slice([1, 2])','    var b = clone(a)','    unwrap(deque.push_back(b, 3))','    print(deque.snapshot(a), deque.snapshot(b), a == deque.from_slice([1, 2]), a == b, a)']
expected.append('[1, 2] [1, 2, 3] true false <Deque:2>')
program.append('}');source='\n'.join(program)+'\n';expected='\n'.join(expected)+'\n'

with tempfile.TemporaryDirectory(prefix='hua-deque-heap-',dir=build) as temp:
    base=Path(temp);src=base/'source';src.mkdir();work=base/'work';work.mkdir();n=0
    def fixture(text):
        global n
        n+=1;p=src/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def triple(text,want):
        p=fixture(text);run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.unlink();run('check',artifact,expected='OK\n')
            run(command,target,cwd=work,expected=want)
        return artifact
    def failure(text,error,static=False):
        p=fixture(text)
        if static:run('check',p,code=1,error=error);return
        run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:run(command,target,cwd=work,code=1,error=error)
    artifact=triple(source,expected)
    assert header_format(artifact)==14,header_format(artifact)
    old=triple('import std.list as list\nfn main() { print(list.len(list.from_slice([1]))) }\n','1\n')
    assert header_format(old)<14,header_format(old)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,13);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    run('run',bad,code=1,error='E6002')
    # E5: a key callback that changes the same List stops the call.
    failure('import std.heap as heap\nimport std.list as list\nvar g = list.from_slice([3, 1, 2])\nfn key(x int) int {\n    unwrap(list.append(g, 9))\n    return x\n}\nfn main() { print(heap.push_by_key(g, 5, key)) }\n','list modified during callback')
    failure('import std.heap as heap\nimport std.list as list\nfn main() {\n    let xs = [1, 2]\n    print(heap.smallest(xs, -1))\n}\n','E4003')
    # Static checks.
    for body,code in [('import std.deque as deque\nfn main() {\n    let d = deque.from_slice([1])\n    print(deque.push_back(d, 2))\n}\n','E3003'),
                      ('import std.deque as deque\nfn take(d Deque<int>) { print(deque.pop_front(d)) }\nfn main() { }\n','E3003'),
                      ('import std.deque as deque\nfn main() {\n    var d = deque.from_slice([1])\n    print(deque.push_back(d, "x"))\n}\n','E3004'),
                      ('import std.deque as deque\nfn main() { print(deque.len([1])) }\n','E3004'),
                      ('import std.heap as heap\nimport std.list as list\nfn main() {\n    var h = list.from_slice([true])\n    heap.heapify(h)\n}\n','E3004'),
                      ('import std.heap as heap\nimport std.list as list\nfn main() {\n    let h = list.from_slice([1])\n    print(heap.pop(h))\n}\n','E3003'),
                      ('import std.heap as heap\nimport std.list as list\nfn key(x int) bool { return true }\nfn main() {\n    var h = list.from_slice([1])\n    print(heap.pop_by_key(h, key))\n}\n','E3004'),
                      ('import std.heap as heap\nimport std.list as list\nfn key(x int) int { return x }\nfn main() {\n    var h = list.from_slice([1])\n    print(heap.push_by_key(h, "s", key))\n}\n','E3004'),
                      ('fn main() {\n    var d Deque = []\n}\n','E3004')]:
        failure(body,code,True)
print(f'deque/heap three-path checks passed: {checks}')
