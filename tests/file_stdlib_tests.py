"""D002-b public File: independent AST, VM and source-free HUAB observations."""
from pathlib import Path
import json,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:])
checks=cases=0
prefix='import std.file as f\nimport std.bytes as b\nimport std.error as e\n'
def run(command,path,*extra,cwd=None,expected=None,error=None,executable=exe):
    global checks
    r=subprocess.run([str(executable),command,str(path),*map(str,extra)],cwd=cwd,capture_output=True,timeout=20)
    out=r.stdout.decode('utf-8').replace('\r\n','\n');err=r.stderr.decode('utf-8')
    assert r.returncode==(1 if error else 0),(command,path,r.returncode,out,err)
    if expected is not None:assert out==expected,(command,path,out,expected,err)
    if error:assert error in err,(command,path,out,err,error)
    checks+=1;return r
with tempfile.TemporaryDirectory(prefix="hua-file-",dir=build) as temporary:
    base=Path(temporary);source=base/'source';source.mkdir()
    def fixture(code):
        global cases
        cases+=1;p=source/f"case{cases}.hua";p.write_text(prefix+code,encoding='utf-8',newline='\n');return p
    def compare(code,expected,initial=None,files=None,version=8):
        p=fixture(code);run('check',p,expected='OK\n');run('build',p)
        artifact=p.with_suffix('.huab');assert struct.unpack_from('<I',artifact.read_bytes(),8)[0]==version
        for engine,command,path in (('AST','interpret',p),('VM','run',p),('HUAB','run',artifact)):
            work=base/f"work{cases}-{engine}";work.mkdir()
            for name,data in (initial or {}).items():(work/name).write_bytes(data)
            if engine=='HUAB':p.unlink();run('check',artifact,expected='OK\n')
            run(command,path,cwd=work,expected=expected)
            for name,data in (files or {}).items():assert (work/name).read_bytes()==data,(engine,name,(work/name).read_bytes(),data)
        return artifact
    def static(code,error='E3008'):
        p=fixture(code)
        for command in ('check','interpret','run','build'):run(command,p,error=error)
        assert not p.with_suffix('.huab').exists()
    def dynamic(code,error):
        p=fixture(code);run('check',p,expected='OK\n');run('build',p)
        for command in ('interpret','run'):run(command,p,cwd=base,error=error)
        p.unlink();run('run',p.with_suffix('.huab'),cwd=base,error=error)

    artifact=compare('''fn main() Result<void,e.Value>{
let h=unwrap(f.open("data.bin","w+"))
let raw=unwrap(b.from_ints([65,0,255,10]))
print(unwrap(f.write(h,raw)),unwrap(f.tell(h)))
print(unwrap(f.seek(h,0,"start")))
let chunk=unwrap(f.read(h,9))
if chunk!=nil {print(unwrap(b.to_ints(chunk)))}
print(is_none(unwrap(f.read(h,1))),b.len(unwrap(unwrap(f.read(h,0)))))
print(unwrap(f.seek(h,-2,"end")),unwrap(f.tell(h)),unwrap(f.seek(h,1,"current")))
f.flush(h)?
f.close(h)?
print(unwrap(f.state(h)),is_ok(f.close(h)),is_none(unwrap(f.close_error(h))))
return ok()
}
''','4 4\n0\n[65, 0, 255, 10]\ntrue 0\n2 2 3\nclosed true true\n',files={'data.bin':bytes([65,0,255,10])})
    compare('''fn main(){
let h=unwrap(f.open("data","a+"))
print(unwrap(f.tell(h)))
f.seek(h,0,"start")
print(unwrap(f.write(h,b.from_text("X"))),unwrap(f.tell(h)))
f.seek(h,0,"start")
let data=unwrap(f.read(h,20))
if data!=nil {print(unwrap(b.to_text(data)))}
f.close(h)
}
''','3\n1 4\noldX\n',initial={'data':b'old'},files={'data':b'oldX'})
    compare('''fn main(){
let r=unwrap(f.open("data","r"))
print(is_none(unwrap(f.read(r,1))))
let w=unwrap(f.open("data","a"))
f.seek(w,0,"start")
f.write(w,b.from_text("grown"))
f.close(w)
let next=unwrap(f.read(r,8))
if next!=nil {print(unwrap(b.to_text(next)))}
f.close(r)
}
''','true\ngrown\n',initial={'data':b''},files={'data':b'grown'})
    for mode,initial,final,start,readable,writable in (
        ('r',b'abc',b'abc',0,True,False),('w',b'abc',b'Z',0,False,True),
        ('a',b'abc',b'abcZ',3,False,True),('r+',b'abc',b'Zbc',0,True,True),
        ('w+',b'abc',b'Z',0,True,True),('a+',b'abc',b'abcZ',3,True,True)):
        compare(f'''fn main(){{
let h=unwrap(f.open("data","{mode}"))
print(unwrap(f.tell(h)),is_ok(f.write(h,b.from_text("Z"))),is_ok(f.read(h,0)))
f.close(h)
}}
''',f'{start} {str(writable).lower()} {str(readable).lower()}\n',initial={'data':initial},files={'data':final})
    compare('''struct Box {handle f.File}
fn main(){
let h=unwrap(f.open("data","w"))
let alias=h
let box=Box{handle:h}
print(type(h),type(box.handle),is_ok(f.close(alias)),unwrap(f.state(box.handle)))
let issue=unwrap_err(f.write(h,b.from_text("x")))
print(type(issue),e.domain(f.error(issue)),e.code(f.error(issue)),f.transferred(issue),f.partial(issue))
}
''','std.file.File std.file.File true closed\nstd.file.IOFailure file closed 0 nil\n',files={'data':b''})
    compare('''fn main(){
print(e.code(unwrap_err(f.open("missing","r"))),e.code(unwrap_err(f.open("data","bad"))))
print(e.code(unwrap_err(f.open("","r"))),is_err(f.open(".","r")))
let h=unwrap(f.open("data","r"))
let issue=unwrap_err(f.write(h,b.from_text("x")))
print(e.code(f.error(issue)),f.transferred(issue),f.partial(issue),e.origin(f.error(issue)))
print(e.code(unwrap_err(f.flush(h))),is_none(unwrap(f.close_error(h))))
f.close(h)
}
''','not_found bad_mode\ninvalid_argument true\npermission_denied 0 nil std.file.write\npermission_denied true\n',initial={'data':b'abc'},files={'data':b'abc'})
    compare('''fn main(){
let h=unwrap(f.open("data","r+"))
f.seek(h,2,"start")
print(is_err(f.seek(h,-3,"current")),unwrap(f.tell(h)),is_err(f.seek(h,9223372036854775807,"end")),unwrap(f.tell(h)))
print(e.code(unwrap_err(f.seek(h,0,"bad"))))
f.seek(h,8,"start")
f.write(h,b.from_text("x"))
f.close(h)
}
''','true 2 true 2\ninvalid_argument\n',initial={'data':b'abc'},files={'data':b'abc'+b'\0'*5+b'x'})
    compare('''fn checked() Result<void,e.Value>{
let h=f.open("data","w")?
defer f.close(h)
f.flush(h)?
return ok()
}
print(checked())
''','ok()\n',files={'data':b''})
    compare('''import std.gc as gc
fn main(){
for i in 0..300 {let h=unwrap(f.open("data","a"))
f.write(h,b.from_text("x"))
gc.collect()}
print("done")
}
''','done\n',files={'data':b'x'*300})
    compare('''import std.list as l
fn main(){
let h=unwrap(f.open("data","r"))
var held=l.from_slice([b.from_text("")])
var count=0
for i in 0..17 {f.seek(h,0,"start")
let r=f.read(h,1048576)
if is_ok(r) {let chunk=unwrap(r)
if chunk!=nil {l.append(held,chunk)}
count++} else {let issue=unwrap_err(r)
print(count,e.code(f.error(issue)),f.transferred(issue),f.partial(issue))}}
l.clear(held)
print(is_ok(f.read(h,1048576)))
f.close(h)
}
''','16 limit 0 nil\ntrue\n',initial={'data':b'x'},files={'data':b'x'})
    compare('''import std.list as l
fn main(){
let first=unwrap(f.open("data","r"))
var handles=l.from_slice([first])
for i in 1..64 {l.append(handles,unwrap(f.open("data","r")))}
print(e.code(unwrap_err(f.open("data","w"))))
l.clear(handles)
print(is_ok(f.open("data","r")))
f.close(first)
}
''','limit\ntrue\n',initial={'data':b'x'},files={'data':b'x'})
    compare('''async fn worker(path string) int{
let h=unwrap(f.open(path,"w"))
f.write(h,b.from_text("child"))
return 5
}
fn main(){
let local=unwrap(f.open("parent","w"))
let t=worker("child")
print(await t,await t,unwrap(f.state(local)))
f.close(local)
}
''','5 5 open\n',files={'child':b'child','parent':b''})
    for code in (
        'fn main(){let h=unwrap(f.open("x","w"))\nclone(h)}\n',
        'struct Box {file f.File}\nfn main(){let h=unwrap(f.open("x","w"))\nclone(Box{file:h})}\n',
        'fn main(){let h=unwrap(f.open("x","w"))\nclone([h])}\n',
        'fn main(){let h=unwrap(f.open("x","w"))\nlet m=map[string]f.File{"a":h}\nclone(m)}\n',
        'import std.list as l\nfn main(){let h=unwrap(f.open("x","w"))\nclone(l.from_slice([h]))}\n',
        'fn main(){let h=f.open("x","w")\nclone(h)}\n',
        'fn use(h f.File) int{return 1}\nfn main(){let h=unwrap(f.open("x","w"))\nlet t=spawn use(h)}\n',
        'struct Box {file f.File}\nfn use(h Box) int{return 1}\nfn main(){let h=unwrap(f.open("x","w"))\nlet t=spawn use(Box{file:h})}\n',
        'async fn bad() f.File{return unwrap(f.open("x","w"))}\nfn main(){let t=bad()}\n'):
        static(code)
    static('let h=f.File{}\n','E3004');static('let issue=f.IOFailure{}\n','E3004')
    dynamic('fn duplicate(v){return clone(v)}\nfn main(){let h=unwrap(f.open("dynamic","w"))\nduplicate(h)}\n','E4007')
    dynamic('fn main(){let h=unwrap(f.open("captured","w"))\nfn local() int{return 1}\nclone(local)}\n','E4007')
    dynamic('let global=unwrap(f.open("global","w"))\nfn worker() int{return 1}\nfn main(){let t=spawn worker()}\n','E4104')
    dynamic('fn main(){let h=unwrap(f.open("captured-task","w"))\nfn local() int{return 1}\nlet t=spawn local()}\n','E4104')
    dynamic('import std.list as l\nfn main(){var empty []f.File=[]\nvar held=l.from_slice(empty)\nfn local() int{return 1}\nlet h=unwrap(f.open("late-capture","w"))\nl.append(held,h)\nclone(local)}\n','E4007')
    dynamic('import std.list as l\nfn main(){var empty []f.File=[]\nvar held=l.from_slice(empty)\nfn local() int{return 1}\nlet h=unwrap(f.open("late-task","w"))\nl.append(held,h)\nlet t=spawn local()}\n','E4104')
    # Unused nominal File types require version 8, independently of std.file function references.
    typed=compare('fn unused(h f.File) int{return 1}\nprint("typed")\n','typed\n')
    forged=base/'downgraded.huab';raw=bytearray(typed.read_bytes());struct.pack_into('<I',raw,8,7);forged.write_bytes(raw)
    for command in ('check','run'):run(command,forged,expected='',error='E6002')
    old=build/'d002-b/hua-old.exe'
    if old.exists():
        for command in ('check','run'):run(command,artifact,expected='',error='E6002',executable=old)
    # Independent controls prove ordinary programs and Error programs retain formats 6 and 7.
    for version,code,expected in ((6,'print(7//2,7/2)\n','3 3.5\n'),(7,'import std.error as e\nprint(e.code(e.from_string("old","")))\n','failure\n')):
        p=source/f'control{version}.hua';p.write_text(code,encoding='utf-8',newline='\n')
        run('build',p);a=p.with_suffix('.huab');assert struct.unpack_from('<I',a.read_bytes(),8)[0]==version
        p.unlink();run('run',a,expected=expected)
        if old.exists():run('run',a,expected=expected,executable=old)
report={'cases':cases,'checks':checks,'engines':['AST','VM','source-free HUAB'],'formats':[6,7,8],'old_runtime_checked':old.exists(),'result':'passed'}
(build/'d002-b').mkdir(exist_ok=True);(build/'d002-b/execution-verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report))
