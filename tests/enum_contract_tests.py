"""BO-04 public enum/value contracts across AST, VM and source-free HUAB."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, struct, subprocess, tempfile, time
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,data):Path(path).write_bytes((json.dumps(data,ensure_ascii=False,indent=2)+"\n").encode("utf-8"))
def text(data):return data.decode("utf-8").replace("\r\n","\n")
CASES=[
{"name":"basic","source":'import std.json as j\nenum E{I(int),Text(string),Empty}\nfn main(){print(E.I(7),E.Text("中文"),E.Empty())\nprint(type(E.I(7)),is_err(j.encode(E.Empty())))}\n',"output":"E.I(7) E.Text(中文) E.Empty()\nE true\n"},
{"name":"optional","source":'enum E{Maybe(int?),Empty}\nfn main(){print(E.Maybe(nil),E.Maybe(3),E.Empty())\nmatch E.Maybe(nil){Maybe(x)=>print(is_none(x))\nEmpty=>print(false)}}\n',"output":"E.Maybe(nil) E.Maybe(3) E.Empty()\ntrue\n"},
{"name":"tuple","source":'enum E{Pair(int,string,bool),Empty}\nfn main(){let e=E.Pair(1,"x",true)\nprint(e)\nmatch e{Pair(a,b,c)=>print(a,b,c)\nEmpty=>print("empty")}}\n',"output":"E.Pair(1, x, true)\n1 x true\n"},
{"name":"record-copy","source":'struct Box{n int}\nenum E{Box(Box),Empty}\nfn main(){var box=Box{n:1}\nlet e=E.Box(box)\nbox.n=9\nvar other=e\nother=E.Empty()\nprint(e,other,box.n)}\n',"output":"E.Box(Box{n: 1}) E.Empty() 9\n"},
{"name":"shared-and-clone","source":'enum E{Array([]int),Empty}\nfn main(){var a=[1,2]\nlet e=E.Array(a)\nlet snapshot=clone(e)\na[0]=9\nprint(e,snapshot)\nmatch e{Array(v)=>print(v)\nEmpty=>print("empty")}}\n',"output":"E.Array([9, 2]) E.Array([1, 2])\n[9, 2]\n"},
{"name":"fixed-array","source":'enum E{Array([2]int),Empty}\nfn main(){var a [2]int=[1,2]\nlet e=E.Array(a)\na[0]=9\nprint(e,a)}\n',"output":"E.Array([1, 2]) [9, 2]\n"},
{"name":"result-payload","source":'enum E{Outcome(Result<int>),Empty}\nfn make(fail bool) Result<E>{if fail{return err("bad")}\nreturn ok(E.Outcome(ok(7)))}\nfn main(){print(unwrap(make(false)),unwrap_err(make(true)),E.Outcome(err("no")))}\n',"output":"E.Outcome(ok(7)) bad E.Outcome(err(no))\n"},
{"name":"nested","source":'enum Inner{N(int),Empty}\nenum Outer{One(Inner),Empty}\nfn main(){let e=Outer.One(Inner.N(8))\nprint(e,clone(e))\nmatch e{One(x)=>{match x{N(n)=>print(n)\nEmpty=>print(0)}}\nEmpty=>print(0)}}\n',"output":"Outer.One(Inner.N(8)) Outer.One(Inner.N(8))\n8\n"},
{"name":"containers","source":'import std.list as l\nimport std.json as j\nenum E{N(int),Empty}\nstruct Box{value E}\nfn main(){let e=E.N(4)\nlet a=[e]\nlet xs=l.from_slice(a)\nlet table=map[string]E{"x":e}\nprint(Box{value:e},a,unwrap(l.get(xs,0)),unwrap(table["x"]))\nprint(is_err(j.encode(a)),is_err(j.encode(table)))}\n',"output":"Box{value: E.N(4)} [E.N(4)] E.N(4) E.N(4)\ntrue true\n"},
{"name":"return-defer","source":'enum E{N(int),Empty}\nvar current=E.N(1)\nfn replace(){current=E.N(9)}\nfn take() E{defer replace()\nreturn current}\nfn main(){print(take(),current)}\n',"output":"E.N(1) E.N(9)\n"},
{"name":"match-once","source":'enum E{N(int),Empty}\nvar count=0\nfn make() E{count+=1\nreturn E.N(7)}\nfn main(){match make(){N(x)=>print(x)\nEmpty=>print(0)}\nprint(count)}\n',"output":"7\n1\n"},
{"name":"module","source":'import api\nfn main(){let e=api.make()\nprint(e)\nmatch e{api.Event.Text(s)=>print(s)\napi.Event.Empty=>print("empty")}}\n',"modules":{"api.hua":'pub enum Event{Text(string),Empty}\npub fn make() Event{return Event.Text("module")}\n'},"output":"Event.Text(module)\nmodule\n"},
{"name":"task-argument","source":'import std.gc as gc\nenum E{Array([]int),Empty}\nasync fn work(e E) E{gc.collect()\nreturn e}\nfn main(){var a=[1,2]\nlet e=E.Array(a)\nlet job=work(e)\na[0]=9\nprint(await job,e)}\n',"output":"E.Array([1, 2]) E.Array([9, 2])\n","workers":[1,2]},
{"name":"task-capture","source":'enum E{N(int),Empty}\nfn main(){var e=E.N(1)\nasync fn work() E{return e}\nlet job=work()\ne=E.N(9)\nprint(await job,e)}\n',"output":"E.N(1) E.N(9)\n","workers":[1,2]},
{"name":"file-alias","source":'import std.file\nimport std.json as j\nenum E{Handle(std.file.File),Empty}\nfn main(){let h=unwrap(std.file.open("enum.bin","w+"))\nlet e=E.Handle(h)\nprint(e,is_err(j.encode(e)))\nprint(is_ok(std.file.close(h)))}\n',"output":"E.Handle(<File>) true\ntrue\n"},
{"name":"entry-failure","source":'enum Failure{Message(string),Empty}\nfn main() Result<void,Failure>{return err(Failure.Message("failure"))}\n',"output":"","runtime_error":"E0003"},
{"name":"clone-file","source":'import std.file\nenum E{Handle(std.file.File),Empty}\nfn main(){let h=unwrap(std.file.open("enum.bin","w+"))\ndefer std.file.close(h)\nclone(E.Handle(h))}\n',"output":"","runtime_error":"E4007"},
{"name":"task-file","source":'import std.file\nenum E{Handle(std.file.File),Empty}\nasync fn work(e E) int{return 1}\nfn main(){let h=unwrap(std.file.open("enum.bin","w+"))\ndefer std.file.close(h)\nprint(await work(E.Handle(h)))}\n',"output":"","runtime_error":"E4104"},
{"name":"readonly-array","source":'enum E{Array([]int),Empty}\nfn write(e E){match e{Array(v)=>v[0]=9\nEmpty=>{}}}\nfn main(){write(E.Array([1]))}\n',"static_error":"E3003"},
{"name":"wrong-payload","source":'enum E{N(int),Empty}\nfn main(){E.N("bad")}\n',"static_error":"E3004"},
{"name":"private-field","source":'enum E{N(int),Empty}\nfn main(){let e=E.N(1)\nprint(e.value)}\n',"static_error":"E3004"},
{"name":"missing-match","source":'enum E{N(int),Empty}\nfn main(){match E.N(1){N(x)=>print(x)}}\n',"static_error":"E3011"}
]
CASES.extend([
{"name":"numeric-void-result","source":'enum E{Small(u8),Real(float),Unit(Result<void>)}\nfn unit() Result<void>{return ok()}\nfn main(){print(E.Small(255),E.Real(2),E.Unit(unit()))}\n',"output":"E.Small(255) E.Real(2) E.Unit(ok())\n"},
{"name":"interface-payload","source":'interface Named{fn name() string}\nstruct Person{}\nfn Person.name() string{return "person"}\nenum E{Object(Named),Empty}\nfn main(){let e=E.Object(Person{})\nprint(e)\nmatch e{Object(x)=>print(x.name())\nEmpty=>print("empty")}}\n',"output":"E.Object(Person{})\nperson\n"}
])
class Audit:
    def __init__(self,root,out,exe,baseline,resume):
        self.root,self.out,self.resume=root,out,resume
        self.executables={"new":exe}
        if baseline:self.executables={"baseline":baseline,**self.executables}
        self.identities={key:sha(path) for key,path in self.executables.items()}
        self.calls=[];self.checks=0;self.completed=[];self.reused=0
        self.env=dict(os.environ);self.env["PATH"]=str(exe.parent)+os.pathsep+self.env.get("PATH","")
    def invoke(self,fixture,label,key,operation,path,expected=None,error=None,code=0,workers=1,artifact=None):
        executable=self.executables[key]
        args=list(map(str,[executable,operation,path]+(["-o",artifact] if artifact else [])))
        signature={"command":args,"exe_sha256":self.identities[key],"input_sha256":sha(path),"fixture":fixture["sha256"],"expected":expected,"error":error,"returncode":code,"workers":workers}
        name=fixture["id"]+"-"+label+"-"+key+"-"+self.identities[key][:8]+f"-w{workers}"
        folder=self.out/"commands";folder.mkdir(exist_ok=True)
        target=folder/(name+".json");stdout=folder/(name+".stdout");stderr=folder/(name+".stderr")
        if target.exists():
            assert self.resume,"evidence exists; explicitly use --resume"
            row=json.loads(target.read_text(encoding="utf-8"))
            assert row["passed"] and row["signature"]==signature,"unchanged failed or mismatched command; investigate before retry"
            assert sha(stdout)==row["stdout_sha256"] and sha(stderr)==row["stderr_sha256"]
            if artifact:assert sha(artifact)==row["artifact_sha256"]
            self.reused+=1
        else:
            env=dict(self.env,HUA_TASK_WORKERS=str(workers));start=time.monotonic()
            try:r=subprocess.run(args,cwd=fixture["work"],env=env,capture_output=True,timeout=20)
            except subprocess.TimeoutExpired as failure:
                stdout.write_bytes(failure.stdout or b"");stderr.write_bytes(failure.stderr or b"")
                save(target,{"passed":False,"signature":signature,"timeout":True,"seconds":time.monotonic()-start});raise
            stdout.write_bytes(r.stdout);stderr.write_bytes(r.stderr)
            row={"signature":signature,"seconds":time.monotonic()-start,"returncode":r.returncode,"timeout":False,"stdout_sha256":sha(stdout),"stderr_sha256":sha(stderr),"passed":False}
            if artifact and artifact.is_file():row["artifact_sha256"]=sha(artifact)
            save(target,row)
            assert r.returncode==code,(name,r.returncode,text(r.stdout),text(r.stderr))
            if expected is not None:assert text(r.stdout)==expected,(name,text(r.stdout),expected,text(r.stderr))
            if error:assert error in text(r.stderr),(name,error,text(r.stderr))
            row["passed"]=True;save(target,row)
        self.calls.append({"evidence":str(target.relative_to(self.out)),"seconds":row["seconds"]})
        self.checks+=1
        return stdout.read_bytes(),stderr.read_bytes()
    def compare(self,outputs):
        if "baseline" in outputs:
            assert outputs["baseline"]==outputs["new"],"full stdout/stderr differ"
            self.checks+=1
    def case(self,case):
        digest=hashlib.sha256(json.dumps(case,sort_keys=True,ensure_ascii=False).encode("utf-8")).hexdigest()
        identity=case["name"]+"-"+digest[:10];folder=self.out/"cases"/identity;folder.mkdir(parents=True,exist_ok=True)
        work=folder/"work";work.mkdir(exist_ok=True)
        files={"main.hua":case["source"],**case.get("modules",{})}
        for name,contents in files.items():(folder/name).write_bytes(contents.encode("utf-8"))
        fixture={"id":identity,"sha256":digest,"work":work}
        source=folder/"main.hua";artifacts={}
        if "static_error" in case:
            for operation in ("check","interpret","run","build"):
                outputs={key:self.invoke(fixture,"static-"+operation,key,operation,source,"",case["static_error"],1) for key in self.executables}
                self.compare(outputs)
            assert not source.with_suffix(".huab").exists();self.checks+=1
        else:
            for operation in ("check","build"):
                for key in self.executables:
                    artifact=folder/(key+"-"+self.identities[key][:8]+".huab")
                    self.invoke(fixture,operation,key,operation,source,expected="OK\n" if operation=="check" else None,artifact=artifact if operation=="build" else None)
                    if operation=="build":artifacts[key]=artifact
            if "baseline" in artifacts:
                assert artifacts["baseline"].read_bytes()==artifacts["new"].read_bytes(),"HUAB bytes changed"
                self.checks+=1
            for workers in case.get("workers",[1]):
                for operation in ("interpret","run"):
                    outputs={key:self.invoke(fixture,"source-"+operation,key,operation,source,case["output"],case.get("runtime_error"),1 if "runtime_error" in case else 0,workers) for key in self.executables}
                    self.compare(outputs)
            for name,contents in files.items():
                source_path=folder/name;held=source_path.with_suffix(".source-held")
                if held.exists():assert held.read_bytes()==contents.encode("utf-8");source_path.unlink()
                else:source_path.rename(held)
                assert not source_path.exists();self.checks+=1
            for producer,artifact in artifacts.items():
                header=struct.unpack_from("<II",artifact.read_bytes(),8);assert header[0] in (6,7,8) and header[1]==1;self.checks+=1
                for key in self.executables:self.invoke(fixture,producer+"-huab-check",key,"check",artifact,"OK\n")
                for workers in case.get("workers",[1]):
                    outputs={key:self.invoke(fixture,producer+"-huab-run",key,"run",artifact,case["output"],case.get("runtime_error"),1 if "runtime_error" in case else 0,workers) for key in self.executables}
                    self.compare(outputs)
        self.completed.append(case["name"]);self.summary("running")
    def summary(self,status,error=None):
        save(self.out/"results.json",{"schema":"hua.enum.contract.v1","status":status,"checks":self.checks,"cases":self.completed,"commands":len(self.calls),"reused_commands":self.reused,"command_seconds":sum(row["seconds"] for row in self.calls),"identities":self.identities,"error":error,"full_own_source_run":False})

def archive_records(blob):
    pos=32;fields=[];instructions=[]
    def u32():
        nonlocal pos
        value=struct.unpack_from("<I",blob,pos)[0];pos+=4;return value
    def byte():
        nonlocal pos
        value=blob[pos];pos+=1;return value
    def string():
        nonlocal pos
        size=u32();start=pos;value=bytes(blob[pos:pos+size]).decode("utf-8");pos+=size
        return value,start
    def span():
        nonlocal pos
        pos+=20
    for _ in range(u32()):string();string()
    for _ in range(u32()):
        owner,_=string();span();byte()
        for _ in range(u32()):
            name,start=string();string();span();fields.append((owner,name,start))
    for _ in range(u32()):
        string();span();byte();byte();byte();byte()
        for _ in range(u32()):string();string();byte();span()
        for _ in range(u32()):string()
    assert u32()==0,"these enum persistence fixtures must not contain external modules"
    def chunk():
        nonlocal pos
        name,_=string()
        for _ in range(u32()):
            start=pos;op=byte();span();label,_=string();string();u32();u32();byte()
            constant=pos;kind=byte();constant_text=None
            if kind==1:byte()
            elif kind in (2,3):pos+=8
            elif kind==4:constant_text,constant_start=string()
            elif kind==15:string();byte();pos+=8
            else:assert kind==0
            for _ in range(u32()):string()
            for _ in range(u32()):byte()
            instructions.append({"chunk":name,"offset":start,"op":op,"text":label,"constant_kind_offset":constant,"constant_kind":kind,"constant_text":constant_text,"constant_start":constant_start if kind==4 else None})
    chunk()
    for _ in range(u32()):chunk()
    assert pos==len(blob)
    return fields,instructions
def archive_checks(audit,seed,expected):
    import zlib
    original=seed.read_bytes();fields,instructions=archive_records(original)
    def publish(name,blob):
        struct.pack_into("<Q",blob,16,len(blob)-32);struct.pack_into("<I",blob,24,zlib.crc32(blob[32:])&0xffffffff)
        folder=audit.out/"cases"/name;folder.mkdir(parents=True,exist_ok=True);work=folder/"work";work.mkdir(exist_ok=True)
        path=folder/(name+".huab");path.write_bytes(blob)
        return {"id":name,"sha256":sha(path),"work":work},path
    v5=bytearray(original);struct.pack_into("<I",v5,8,5)
    fixture,path=publish("legacy-huab5",v5)
    outputs={key:audit.invoke(fixture,"check",key,"check",path,"OK\n") for key in audit.executables};audit.compare(outputs)
    outputs={key:audit.invoke(fixture,"run",key,"run",path,expected) for key in audit.executables};audit.compare(outputs)
    audit.completed.append("legacy-huab5")
    owner,name,start=next(row for row in fields if row[1]=="$Box#0#Box")
    invalid=bytearray(original);invalid[start+len("$Box#")]=ord("1")
    fixture,path=publish("invalid-enum-layout-gap",invalid)
    if "baseline" in audit.executables:audit.invoke(fixture,"check","baseline","check",path,"OK\n")
    audit.invoke(fixture,"check","new","check",path,"","E6002",1)
    audit.completed.append("invalid-enum-layout-gap")
    constructor=[r for r in instructions if r["chunk"]=="E.Box"]
    tag=next(r for r in constructor if r["constant_text"]=="Box")
    active=next(r for r in constructor if r["op"]==30 and r["text"]=="$Box#0#Box")
    argument=next(r for r in constructor if r["offset"]<active["offset"] and r["op"]==1)
    mutations={}
    blob=bytearray(original);blob[tag["constant_start"]:tag["constant_start"]+3]=b"Bad";mutations["invalid-enum-tag"]=blob
    blob=bytearray(original);blob[active["offset"]]=3;mutations["invalid-enum-missing-payload"]=blob
    blob=bytearray(original);blob[argument["offset"]]=0
    assert argument["constant_kind"]==0
    at=argument["constant_kind_offset"];blob[at:at+1]=b"\x04"+struct.pack("<I",3)+b"bad";mutations["invalid-enum-payload-type"]=blob
    for name,blob in mutations.items():
        fixture,path=publish(name,blob)
        for key in audit.executables:audit.invoke(fixture,"check",key,"check",path,"OK\n")
        if "baseline" in audit.executables:audit.invoke(fixture,"run","baseline","run",path)
        audit.invoke(fixture,"run","new","run",path,"","E4003",1)
        audit.completed.append(name)
    audit.summary("passed")

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("exe",type=Path);parser.add_argument("root",type=Path);parser.add_argument("build",type=Path);parser.add_argument("--baseline",type=Path);parser.add_argument("--output",type=Path);parser.add_argument("--resume",action="store_true")
    args=parser.parse_args();root=args.root.resolve();build=args.build.resolve()
    out=args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="hua-enum-contract-",dir=build))
    assert out.is_relative_to(build) and out!=build
    if out.exists():assert args.resume or not any(out.iterdir()),"choose fresh evidence or explicit --resume"
    out.mkdir(parents=True,exist_ok=True)
    if args.resume and (out/"results.json").exists():shutil.copy2(out/"results.json",out/("results-before-resume-"+str(time.time_ns())+".json"))
    audit=Audit(root,out,args.exe.resolve(),args.baseline.resolve() if args.baseline else None,args.resume)
    try:
        for case in CASES:audit.case(case)
        seed=next((out/"cases").glob("record-copy-*/new-"+audit.identities["new"][:8]+".huab"))
        archive_checks(audit,seed,next(c["output"] for c in CASES if c["name"]=="record-copy"))
        assert all(sha(path)==audit.identities[key] for key,path in audit.executables.items())
        audit.summary("passed")
    except BaseException as failure:audit.summary("stopped",repr(failure));raise
    print(json.dumps({"status":"passed","cases":len(audit.completed),"checks":audit.checks,"commands":len(audit.calls),"reused":audit.reused,"evidence":str(out)},ensure_ascii=False))
if __name__=="__main__":main()
