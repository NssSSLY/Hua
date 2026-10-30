"""D002-c/E5: independent AST, VM and source-free HUAB, on one CPU worker."""
from pathlib import Path
import os,sys,tempfile,subprocess,struct,json
exe,root,build=(Path(x).resolve() for x in sys.argv[1:])
checks=cases=0
legacy=build/"d002-c/hua-before.exe"
legacy=legacy if legacy.is_file() else None
env=dict(os.environ,HUA_TASK_WORKERS="1")
with tempfile.TemporaryDirectory(prefix="hua-resume-",dir=build) as temp:
    base=Path(temp)
    def run(command,path,output=None,error=None,cwd=None,environment=env,executable=exe):
        global checks
        r=subprocess.run([str(executable),command,str(path)],capture_output=True,encoding="utf8",timeout=25,cwd=cwd,env=environment)
        assert r.returncode==(1 if error else 0),(command,r.returncode,r.stdout,r.stderr)
        if output is not None:assert r.stdout==output,(command,r.stdout,output,r.stderr)
        if error:assert error in r.stderr,(command,r.stderr,error)
        checks+=1;return r
    def compare(code,output,error=None,files=None,initial=None,version=6,old_compatible=True):
        global cases,checks
        cases+=1;source=base/f"case{cases}.hua";source.write_text(code,encoding="utf8",newline="\n")
        run("check",source,"OK\n");run("build",source)
        archive=source.with_suffix(".huab")
        assert struct.unpack_from("<II",archive.read_bytes(),8)==(version,1);checks+=1
        for engine,command,path in (("AST","interpret",source),("VM","run",source),("HUAB","run",archive)):
            work=base/f"{cases}-{engine}";work.mkdir()
            for name,data in (initial or {}).items():(work/name).write_bytes(data)
            if engine=="HUAB":source.unlink();run("check",archive,"OK\n")
            run(command,path,output,error,work)
            for name,data in (files or {}).items():assert (work/name).read_bytes()==data,(engine,name)
        if legacy and old_compatible:
            work=base/f"{cases}-old-HUAB";work.mkdir()
            for name,data in (initial or {}).items():(work/name).write_bytes(data)
            run("run",archive,output,error,work,executable=legacy)
            for name,data in (files or {}).items():assert (work/name).read_bytes()==data,("old-HUAB",name)
        return archive
    # S2-1: assignment targets survive recursion and suspension with GC.
    compare("""import std.task as t
import std.gc as gc
struct Box{x int}
fn recurse(n int) int {if n==0 {gc.collect()
return 7}
return recurse(n-1)+1}
async fn delayed(x int) int {t.sleep(1)
gc.collect()
return x}
async fn worker() int {var xs=[0,0]
xs[0]=recurse(40)
xs[1]=await delayed(19)
var box=Box{x:0}
box.x=await delayed(xs[0]+xs[1])
var items=map[string]int{}
items["answer"]=await delayed(box.x)
return unwrap(items["answer"])}
fn main(){print(await worker())}
""","66\n")
    compare("""import std.task as t
fn nested(a Task<int>) int {return await a+1}
async fn leaf() int {t.sleep(1)
print("leaf")
return 3}
async fn parent() int {let a=leaf()
let x=nested(a)
print(await a)
return x}
fn main(){let a=parent()
print(await a,await a)}
""","leaf\n3\n4 4\n")
    compare("""import std.task as t
import std.gc as gc
async fn worker() int {
var a=[2,4,6]
for i in 0..100 {t.yield()
gc.collect()
a[0]+=1}
return a[0]+a[2]}
fn main(){print(await worker())}
""","108\n")
    compare("""import std.task as t
async fn leaf() int {t.sleep(2)
return 8}
fn clean(){print("cleanup",await leaf())}
async fn worker() int {defer clean()
t.yield()
return 5}
fn main(){print(await worker())}
""","cleanup 8\n5\n")
    compare("""import std.task as t
fn cleanup(){t.sleep(1)
print("cleanup")}
async fn worker() int {defer cleanup()
t.sleep(1000)
return 1}
fn main(){let a=worker()
t.sleep(10)
print(t.timeout(a,1),t.state(a))}
""","cleanup\nerr(timeout) canceled\n")
    compare("""import std.task as t
fn cleanup(){t.sleep(1)
print("original-cleanup")}
async fn worker() int {defer cleanup()
t.yield()
return 1//0}
fn main(){await worker()}
""","original-cleanup\n","E4004")
    compare("""import std.task as t
async fn child() int {t.sleep(1000)
return 1}
fn inner() int {let a=child()
return await a}
async fn parent() int {defer print("parent-clean")
return inner()}
fn main(){let a=parent()
t.sleep(10)
print(t.timeout(a,1),t.state(a))}
""","parent-clean\nerr(timeout) canceled\n")
    compare("""import std.task as t
async fn worker(i int) int {for j in 0..8 {t.yield()}
print(i)
return i}
fn main(){let a=worker(1)
let b=worker(2)
print(await b)
print(await a,await b)}
""","2\n2\n1\n1 2\n")
    compare("""import std.task as t
async fn worker(i int) int {t.yield()
return i}
fn main(){let a=worker(1)
let b=worker(2)
print(await t.all(a,b))
print(await t.race(a,b),await a,await b)}
""","[1, 2]\n1 1 2\n")
    compare("""import std.task as t
async fn work() int {var x=0
while true {x+=1
t.yield()}
return x}
fn main(){await work()}
""","","E4099")
    compare("""import std.task as t
fn recur(n int) int {t.yield()
return recur(n+1)}
async fn work() int {return recur(0)}
fn main(){await work()}
""","","E4099")
    compare("""import std.task as t
async fn leaf() int {t.sleep(1)
return 4}
async fn run() int {fn ordinary(a Task<int>) int {return await a+2}
return ordinary(leaf())}
async fn main(){print(await run())}
""","6\n")
    compare("""import std.task as t
fn main(){var a=[1,2,3,4]
var out=[0,0,0,0]
parallel for i in 0..4 {out[i]=a[i]*a[i]}
print(out)}
""","[1, 4, 9, 16]\n")
    compare("""import std.file as f
import std.bytes as b
import std.task as t
async fn work(path string) int {
let h=unwrap(f.open(path,"w+"))
defer f.close(h)
t.yield()
let n=unwrap(f.write(h,b.from_text("Hua")))
f.flush(h)
f.seek(h,0,"start")
t.sleep(1)
let data=unwrap(f.read(h,8))
if data!=nil {print(unwrap(b.to_text(data)))}
f.close(h)
return n}
fn main(){let a=work("one")
let c=work("two")
print(await c,await a)}
""","Hua\nHua\n3 3\n",files={"one":b"Hua","two":b"Hua"},version=8)
    compare("""import std.file as f
import std.bytes as b
import std.task as t
async fn work() int {
let h=unwrap(f.open("once","w"))
defer f.close(h)
f.write(h,b.from_text("once"))
t.sleep(1000)
return 1}
fn main(){let a=work()
t.sleep(20)
print(t.timeout(a,1),t.state(a))}
""","err(timeout) canceled\n",files={"once":b"once"},version=8)
    compare("""import std.file as f
import std.bytes as b
import std.task as t
async fn work() int {
let h=unwrap(f.open("source","r"))
defer f.close(h)
let data=unwrap(f.read(h,1024))
t.yield()
if data!=nil {return b.len(data)}
return 0}
fn main(){print(await work())}
""","4\n",initial={"source":b"\0\xffab"},version=8)
    compare("""import std.task as t
async fn cleanup_child() int {t.sleep(1)
print("cleanup-child")
return 9}
fn cleanup(){print("cleanup",await cleanup_child())}
async fn work() int {defer cleanup()
t.sleep(1000)
return 1}
fn main(){let a=work()
t.sleep(20)
print(t.timeout(a,1),t.state(a))}
""","cleanup-child\ncleanup 9\nerr(timeout) canceled\n",old_compatible=False)
    artifact=compare("fn main(){print(3)}\n","3\n")
    for value in ("0","99","bad"):
        bad=dict(env,HUA_TASK_WORKERS=value)
        # Scheduler is initialized only for programs which create a Task.
        p=base/"invalid-workers.hua";p.write_text("async fn f() int{return 1}\nfn main(){print(await f())}\n",encoding="utf8")
        r=subprocess.run([str(exe),"run",str(p)],env=bad,capture_output=True,encoding="utf8",timeout=10)
        assert r.returncode!=0 and "HUA_TASK_WORKERS" in r.stderr,(value,r.stdout,r.stderr);checks+=1
(build/"d002-c").mkdir(exist_ok=True)
(build/"d002-c/public-verification.json").write_text(json.dumps({"cases":cases,"checks":checks,"workers":1,"paths":["AST","VM","HUAB"],"legacy_reader":bool(legacy),"posix":os.name=="posix"}),encoding="utf8")
print(f"D002-c: {cases} cases, {checks} checks; AST/VM/source-free HUAB; 1 worker")
