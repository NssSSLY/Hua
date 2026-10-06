# Phase 10: interpreter, VM and source-independent HUAB contracts.
import pathlib,subprocess,sys,tempfile,struct,shutil
exe=pathlib.Path(sys.argv[1]);build=pathlib.Path(sys.argv[2]);checks=0;cases=0

def run(command,path,output=None,error=None):
    global checks
    p=subprocess.run([str(exe),command,str(path)],capture_output=True,encoding='utf-8',timeout=20)
    assert p.returncode==(1 if error else 0),(command,path,p.returncode,p.stdout,p.stderr)
    if output is not None:assert p.stdout==output,(command,path,p.stdout,output,p.stderr)
    if error:assert error in p.stderr,(command,path,p.stderr,error)
    checks+=1;return p
with tempfile.TemporaryDirectory(prefix='hua-concurrency-',dir=build) as directory:
    root=pathlib.Path(directory)
    def fixture(text,modules=None):
        global cases
        cases+=1;folder=root/str(cases);folder.mkdir();p=folder/'main.hua';p.write_text(text,encoding='utf-8',newline='\n')
        for name,source in (modules or {}).items():(folder/name).write_text(source,encoding='utf-8',newline='\n')
        return p
    def triple(text,output,error=None,modules=None):
        global checks
        p=fixture(text,modules);run('check',p,'OK\n')
        for engine in ('interpret','run'):run(engine,p,output,error)
        run('build',p);a=p.with_suffix('.huab');assert struct.unpack_from('<II',a.read_bytes(),8)==(6,1);checks+=1
        for source in p.parent.glob('*.hua'):source.unlink()
        run('check',a,'OK\n');run('run',a,output,error);return a
    def invalid(text,error='E3004'):
        p=fixture(text)
        for command in ('check','run','interpret','build'):run(command,p,error=error)
    triple('''import std.task as t
async fn work(x int) int {t.sleep(15)
return x+1}
fn main(){let a Task<int> = work(4)
let b=spawn work(8)
print(type(a),await a,await b,await a)
print(t.done(a),t.state(a),t.cancel(a))}
''','Task<int> 5 9 5\ntrue completed false\n')
    triple('''import std.task as t
fn square(x int) int{return x*x}
fn main(){let a=spawn square(7)
print(await a)
let id=spawn identity()
print(await id != t.worker_id())}
fn identity() string{return t.worker_id()}
''','49\ntrue\n')
    triple('''import std.task as t
async fn work(x int) int{t.sleep(8)
return x}
fn main(){let a=work(3)
let b=work(5)
let pair=t.all(a,b)
print(await pair)
print(await a,await b)}
''','[3, 5]\n3 5\n')
    triple('''import std.task as t
async fn work(x int,ms int) int{t.sleep(ms)
return x}
fn main(){let a=work(7,0)
print(await a)
let b=work(9,500)
print(await t.race(a,b),t.done(b),t.state(b))}
''','7\n7 true canceled\n')
    triple('''import std.task as t
async fn work() int{t.sleep(1000)
return 1}
fn main(){let a=work()
print(t.timeout(a,1))
print(t.done(a),t.state(a))}
''','err(timeout)\ntrue canceled\n')
    triple('''import std.task as t
async fn work() int{return 8}
fn main(){let a=work()
print(await a)
print(t.timeout(a,0))}
''','8\nok(8)\n')
    triple('''import std.task as t
async fn work() int{t.sleep(1000)
return 1}
fn main(){let a=work()
print(t.cancel(a))
print(await a)}
''','true\n','E4101')
    triple('''import std.task as t
async fn broken() int{return 1//0}
fn main(){let a=broken()
let r=t.timeout(a,1000)
print(is_err(r),t.state(a),t.done(a))}
''','true failed true\n')
    for tail in ('print(await broken())','broken()'):
        triple('async fn broken() int{return 1//0}\nfn main(){'+tail+'}\n','','E4004')
    triple('''async fn work() int{defer print("clean")
return 4}
fn main(){taskgroup {spawn work()}
print("after")}
''','clean\nafter\n')
    triple('''async fn work() int{defer print("nested")
return 1}
fn f() int{taskgroup {taskgroup {spawn work()}
return 7}}
fn main(){print(f())}
''','nested\n7\n')
    triple('''import std.task as t
async fn work(){t.sleep(500)
print("late")}
fn main(){taskgroup {work()
let x=1//0}}
''','','E4004')
    triple('''import std.task as t
async fn work(){t.sleep(5)
print("done")}
async fn main(){await work()}
''','done\n')
    triple('''async fn work() int{return 4}
fn main(){let x=3
async fn local(y int) int{return x+y}
print(await local(2),await work())}
''','5 4\n')
    triple('''async fn id<T>(x T) T{return x}
fn main(){print(await id<int>(4),await id<string>("hua"))}
''','4 hua\n')
    triple('''import lib
fn main(){print(await lib.work(6))}
''','12\n',modules={'lib.hua':'pub async fn work(x int) int{return x*2}\n'})
    triple('''async fn work() Result<int> {return ok(5)}
fn f() Result<int> {let x=await work()?
return ok(x+1)}
fn main(){print(f())}
''','ok(6)\n')
    triple('''var values=[1,2]
async fn read() int{return values[0]}
fn main(){let a=read()
values[0]=9
print(await a,values[0])}
''','1 9\n')
    triple('''var values=[1,2]
async fn write(){values[0]=9}
fn main(){await write()}
''','','E4007')
    triple('''fn mutate(mut values []int){values[0]=9}
async fn write(){var own=[1,2]
mutate(own)
print(own)}
fn main(){await write()}
''','[9, 2]\n')
    triple('''import std.gc as gc
fn main(){var values=[1,2,3]
let before=gc.allocated()
for i in 0..600 {let copy=clone(values)}
print(gc.allocated()>before)
print(gc.collect()>=0,gc.live()>0)
print(values)}
''','true\ntrue true\n[1, 2, 3]\n')
    for mode in ('parallel','simd','parallel simd'):
        triple('''fn main(){let a=[1,2,3,4]
var out=[0,0,0,0]
'''+mode+''' for i in 0..4 {out[i]=a[i]*2+1}
print(out)}
''','[3, 5, 7, 9]\n')
    triple('''fn main(){var out=[1,2,3,4,5]
parallel for i in 4..=0 by -2 {out[i]=out[i]*3}
print(out)
parallel for i in 0..0 {out[i]=out[i]*2}
print(out)}
''','[3, 2, 9, 4, 15]\n[3, 2, 9, 4, 15]\n')
    triple('''var count=0
fn bound() int{count++
return 3}
fn main(){var out=[0,0,0]
parallel for i in 0..bound(){out[i]=i}
print(out,count)}
''','[0, 1, 2] 1\n')
    triple('''fn main(){var out [4]f32=[0,0,0,0]
let data [4]f32=[1,2,3,4]
parallel simd for i in 0..4 {out[i]=data[i]*f32(2)}
print(out)}
''','[2, 4, 6, 8]\n')
    triple('''fn main(){var out=[0,0]
parallel for i in 0..3 {out[i]=i}}
''','','E4006')
    triple('''fn main(){var out=[0,0]
parallel for i in 0..2 by 0 {out[i]=i}}
''','','E4005')
    triple('''fn main(){var out=[0,0]
let data=[1,0]
parallel for i in 0..2 {out[i]=4//data[i]}}
''','','E4004')
    invalid('fn main(){print(await 1)}\n')
    invalid('import std.task as t\nfn main(){print(t.cancel(1))}\n')
    invalid('async fn f(mut x []int) {}\nfn main(){}\n','E3011')
    invalid('fn f(mut x []int) {}\nfn main(){var x=[1]\nspawn f(x)}\n','E3003')
    invalid('async fn f() (int,int){return 1,2}\nfn main(){}\n','E3011')
    invalid('import std.task as t\nasync fn f(){}\nfn main(){t.timeout(f(),1)}\n')
    invalid('import std.task as t\nasync fn a() int{return 1}\nasync fn b() string{return "s"}\nfn main(){t.all(a(),b())}\n')
    for code in ['parallel for i in 0..2 {out[i]+=1}', 'parallel for i in 0..2 {out[0]=i}', 'parallel for i in 0..2 {out[i]=out[i+1]}','simd for i in 0..2 {print(i)}','parallel for i in out {out[i]=i}']:
        invalid('fn main(){var out=[0,0]\n'+code+'}\n','E3011')
    invalid('fn main(){var out=["a","b"]\nsimd for i in 0..2 {out[i]=out[i]}}\n')
    invalid('fn main(){let out=[0,0]\nparallel for i in 0..2 {out[i]=i}}\n','E3003')
    for ms in (-1,86400001):
        triple('import std.task as t\nfn main(){t.sleep('+str(ms)+')}\n','','E4103')
    archive=triple('async fn f() int{return 7}\nfn main(){print(await f())}\n','7\n')
    original=archive.read_bytes()
    for version in range(1,6):
        b=bytearray(original);struct.pack_into('<I',b,8,version);archive.write_bytes(b);run('check',archive,error='E6002')
    archive.write_bytes(original)
    triple('''import std.list as list
import std.gc as gc
fn main(){var data=list.from_slice([0])
for i in 1..1200 {list.append(data,i)}
var out=clone(list.snapshot(data))
parallel for i in 0..1200 {out[i]=out[i]*2}
print(out[0],out[1199],gc.collect()>=0)}
''','0 2398 true\n')
    for element in ('f32','f64','float'):
        triple('import std.simd as s\nfn main(){let a [5]'+element+'=[1,2,3,4,5]\nlet b [5]'+element+'=[2,2,2,2,2]\nprint(unwrap(s.add(a,b)),unwrap(s.sub(a,b)),unwrap(s.mul(a,b)))\nprint(s.backend()=="sse2"||s.backend()=="scalar")}\n','[3, 4, 5, 6, 7] [-1, 0, 1, 2, 3] [2, 4, 6, 8, 10]\ntrue\n')
    triple('import std.simd as s\nfn main(){let a [2]f32=[1,2]\nlet b [1]f32=[1]\nprint(is_err(s.add(a,b)))\nprint(unwrap(s.mul(a[0:0],a[0:0])))}\n','true\n[]\n')
    triple('import std.simd as s\nfn main(){let a [1]f32=[3e38]\nprint(is_err(s.mul(a,a)))}\n','true\n')
    invalid('import std.simd as s\nfn main(){s.add([1],[2])}\n')
    # A marker proves the defer was registered before cancellation/timeout.
    for mode in ('cancel','timeout'):
        marker=(root/('ready-'+mode)).as_posix()
        text='import std.task as t\nimport std.fs as fs\nasync fn work() int{defer print("clean")\nunwrap(fs.write_text("'+marker+'","ready"))\nt.sleep(1000)\nreturn 1}\nfn main(){let a=work()\nwhile !unwrap(fs.exists("'+marker+'")){t.sleep(1)}\n'
        if mode=='cancel':text+='print(t.cancel(a))\nprint(await a)}\n';expected='true\nclean\n';error='E4101'
        else:text+='print(t.timeout(a,1))\nprint(t.done(a))}\n';expected='clean\nerr(timeout)\ntrue\n';error=None
        # Separate paths for each execution avoid a stale marker creating a false synchronization proof.
        p=fixture(text);run('check',p,'OK\n')
        for command in ('interpret','run'):
            pathlib.Path(marker).unlink(missing_ok=True);run(command,p,expected,error)
        run('build',p);p.unlink();pathlib.Path(marker).unlink(missing_ok=True);run('run',p.with_suffix('.huab'),expected,error)
    # Parent cancellation reaches a sleeping descendant and still executes registered cleanup.
    marker=(root/'descendant-ready').as_posix()
    p=fixture('import std.task as t\nimport std.fs as fs\nasync fn inner() int{defer print("inner clean")\nunwrap(fs.write_text("'+marker+'","ready"))\nt.sleep(1000)\nreturn 1}\nasync fn outer() int{defer print("outer clean")\nreturn await inner()}\nfn main(){let a=outer()\nwhile !unwrap(fs.exists("'+marker+'")){t.sleep(1)}\nprint(t.timeout(a,1))}\n')
    run('check',p,'OK\n')
    for command in ('interpret','run'):
        pathlib.Path(marker).unlink(missing_ok=True);run(command,p,'inner clean\nouter clean\nerr(timeout)\n')
    run('build',p);p.unlink();pathlib.Path(marker).unlink(missing_ok=True);run('run',p.with_suffix('.huab'),'inner clean\nouter clean\nerr(timeout)\n')
    # Stateful native calls are rejected even when reached indirectly through a Hua wrapper.
    extension=(build/'extensions').as_posix()
    p=fixture('import ext\nfn helper() int{return ext.add(1,2)}\nasync fn work() int{return helper()}\nfn main(){await work()}\n',{'ext.huam':(build/'extensions/native_math.huam').read_text().replace('native_math.dll',extension+'/native_math.dll')})
    shutil.copy2(build/'extensions/native_math.dll',p.parent/'ext.dll')
    for command in ('interpret','run'):run(command,p,'','E4104')
    run('build',p);p.unlink();run('run',p.with_suffix('.huab'),'','E4104')
    triple('async fn inner() int{return 6}\nasync fn outer() Task<int>{return inner()}\nfn main(){let a=outer()\nlet b=await a\nprint(type(a),type(b),await b)}\n','Task<Task<int>> Task<int> 6\n')
    invalid('async fn f(){return 1}\nfn main(){f()}\n','E3011')
    invalid('fn f(){return 1}\nfn main(){spawn f()}\n')
    invalid('struct Item{value int}\nasync fn Item.get() int{return self.value}\nfn main(){}\n','E3011')
    invalid('async fn f() i8{return i8(1)}\nfn main(){let a Task<i64>=f()}\n')
    triple('let value=8\nasync fn use(cb) int{return cb()}\nfn main(){fn callback() int{return value}\nprint(await use(callback))}\n','8\n')
    triple('fn main(){var out [3]i8=[0,0,0]\nparallel for i in 0..3 {out[i]=1}\nprint(out)}\n','[1, 1, 1]\n')
    invalid('fn main(){var out [3]i8=[0,0,0]\nlet data=[1,2,3]\nparallel for i in 0..3 {out[i]=data[i]}}\n')
    invalid('fn f(){}\nfn main(){parallel_map(f,0,1,1,[0],false,false)}\n','E3008')
print(f'{cases} concurrency/GC cases, {checks} checks OK')
