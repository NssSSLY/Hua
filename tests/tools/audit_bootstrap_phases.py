"""BO-00/01: freeze identities and calibrate nested compiler phase diagnostics.
No full own-source run. All experimental input/output remains under a fresh build directory.
"""
from pathlib import Path
import argparse, ctypes, ctypes.wintypes as w, hashlib, json, os, platform, re, shutil, subprocess, sys, time, unittest
from collections import defaultdict
SCHEMA="hua.bootstrap.phase-audit.v1"
RESUME=None
def program_text(data):return data.decode("utf-8").replace("\r\n","\n")
def digest(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,data): Path(p).write_text(json.dumps(data,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
def closure(entry):
    found={}
    queue=[entry]
    while queue:
        p=queue.pop().resolve()
        if p.name in found:
            assert found[p.name]==p,"ambiguous basename"
            continue
        found[p.name]=p
        for name in re.findall(r"^import ([A-Za-z_][\w.]*)",p.read_text(encoding="utf-8"),re.M):
            if not name.startswith("std."): queue.append(p.parent/(name.replace(".","/")+".hua"))
    return dict(sorted(found.items()))
def machine():
    host={"platform":platform.platform(),"python":sys.version,"logical_processors":os.cpu_count(),"processor":os.environ.get("PROCESSOR_IDENTIFIER"),"memory_bytes":None,"cpu_name":None}
    if os.name=="nt":
        class MEMORY(ctypes.Structure):
            _fields_=[("length",w.DWORD),("load",w.DWORD)]+[(name,ctypes.c_ulonglong) for name in ("total_physical","available_physical","total_page","available_page","total_virtual","available_virtual","available_extended")]
        m=MEMORY();m.length=ctypes.sizeof(m)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):host["memory_bytes"]=m.total_physical
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:host["cpu_name"]=winreg.QueryValueEx(key,"ProcessorNameString")[0].strip()
        except OSError:pass
    return host
def freeze(root,out,native):
    assert not out.exists(),"choose a new evidence directory"
    out.mkdir(parents=True)
    files={}
    names=subprocess.check_output(["git","ls-files","-z","--cached","--others","--exclude-standard"],cwd=root).split(b"\0")
    for raw in names:
        if raw:
            rel=raw.decode("utf-8");p=root/rel
            if p.is_file():files[rel]=digest(p)
    sources={}
    for rel,sha in files.items():
        p=Path(rel)
        if p.suffix in (".hua",".cpp",".hpp",".py",".ps1") or rel in ("CMakeLists.txt","selfhost/contract.json","tests/data/a07_completion_cases.json"):
            target=out/"frozen/repository"/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/rel,target)
            sources[rel]={"sha256":sha,"bytes":(root/rel).stat().st_size}
    imported=closure(root/"selfhost/archive_main.hua")
    hashes={name:digest(p) for name,p in imported.items()}
    evidence=root/"tests/artifacts/build/a11-performance/own-formal-context/c07-budget-bootstrap"
    prior=json.loads((evidence/"self-scale-results.json").read_text(encoding="utf-8"))
    assert prior["compiler_sources_identical"] and hashes==prior["compiler_source_hashes"],"prior bootstrap is not current"
    assert digest(native)==prior["runtime_sha256"]
    component=evidence/"bootstrap-component.huab"
    assert digest(component)==prior["bootstrap_component"]["sha256"]
    shutil.copy2(component,out/"baseline-component.huab")
    caches={}
    for name in ("build","build-a11-profile"):
        p=root/name/"CMakeCache.txt"
        caches[name]={"sha256":digest(p),"options":[line for line in p.read_text(encoding="utf-8").splitlines() if re.match(r"(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS_RELEASE|CMAKE_MAKE_PROGRAM|HUA_INTERNAL_PROFILE|HUA_ENABLE_PYTHON_BRIDGE):",line)]}
    data={"schema":SCHEMA,"status":"frozen","created_utc":time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime()),"host":machine(),"git_head":subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip(),"git_branch":subprocess.check_output(["git","branch","--show-current"],cwd=root,text=True).strip(),"git_status":subprocess.check_output(["git","-c","core.quotepath=false","status","--short"],cwd=root,text=True,encoding="utf-8").splitlines(),"files":files,"source_snapshot":sources,"compiler_sources":hashes,"compiler_input_bytes":sum(p.stat().st_size for p in imported.values()),"native":{"path":str(native),"sha256":digest(native)},"baseline_component":{"sha256":digest(out/"baseline-component.huab"),"origin":str(component),"reused_verified_component":True},"builds":caches,"cpu_sampling":{"wpr_installed":bool(shutil.which("wpr")),"used":False,"reason":"Use coarse exclusive phase wall/process CPU and exact GC wall timers first; no system-wide ETW collection. No native stack profile is claimed."},"full_own_source_run":False}
    save(out/"baseline-manifest.json",data)
    print(json.dumps({"status":"frozen","sources":len(sources),"files":len(files),"compiler_modules":len(hashes),"compiler_input_bytes":data["compiler_input_bytes"],"host":data["host"]},ensure_ascii=False))
class Memory(ctypes.Structure):
    _fields_=[("cb",w.DWORD),("PageFaultCount",w.DWORD)]+[(n,ctypes.c_size_t) for n in ("PeakWorkingSetSize","WorkingSetSize","QuotaPeakPagedPoolUsage","QuotaPagedPoolUsage","QuotaPeakNonPagedPoolUsage","QuotaNonPagedPoolUsage","PagefileUsage","PeakPagefileUsage")]
def command(root,out,label,args,limit=30):
    target=out/"commands"/label
    if target.with_suffix(".json").exists():
        assert RESUME is not None and label in RESUME["completed_labels"],"evidence already exists: "+label
        row=json.loads(target.with_suffix(".json").read_text(encoding="utf-8"))
        assert row["command"]==list(map(str,args)) and row["working_directory"]==str(root)
        assert row["returncode"]==0 and not row["timeout"]
        stdout=target.with_suffix(".stdout").read_bytes();stderr=target.with_suffix(".stderr").read_bytes()
        assert len(stdout)==row["stdout_bytes"] and len(stderr)==row["stderr_bytes"]
        print(json.dumps({"reused_completed_command":label,"executed_again":False}),flush=True)
        return row,stdout,stderr
    target.parent.mkdir(exist_ok=True)
    start=time.monotonic();p=subprocess.Popen(list(map(str,args)),cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE,**({"start_new_session":True} if os.name!="nt" else {}))
    timeout=False
    try:stdout,stderr=p.communicate(timeout=limit)
    except subprocess.TimeoutExpired:
        timeout=True
        if os.name=="nt":subprocess.run(["taskkill","/F","/T","/PID",str(p.pid)],capture_output=True,timeout=5)
        else:
            import signal
            os.killpg(p.pid,signal.SIGKILL)
        if p.poll() is None:p.kill()
        stdout,stderr=p.communicate()
    wall=time.monotonic()-start;cpu=None;peak=None
    if os.name=="nt":
        ft=[w.FILETIME() for _ in range(4)]
        if ctypes.windll.kernel32.GetProcessTimes(w.HANDLE(int(p._handle)),*[ctypes.byref(f) for f in ft]):
            cpu=sum((f.dwHighDateTime<<32)|f.dwLowDateTime for f in ft[2:])/1e7
        m=Memory();m.cb=ctypes.sizeof(m)
        if ctypes.windll.psapi.GetProcessMemoryInfo(w.HANDLE(int(p._handle)),ctypes.byref(m),m.cb):peak=m.PeakWorkingSetSize
    row={"label":label,"command":list(map(str,args)),"working_directory":str(root),"limit_seconds":limit,"seconds":wall,"cpu_seconds":cpu,"peak_working_bytes":peak,"returncode":p.returncode,"timeout":timeout,"stdout_bytes":len(stdout),"stderr_bytes":len(stderr)}
    target.with_suffix(".stdout").write_bytes(stdout);target.with_suffix(".stderr").write_bytes(stderr);save(target.with_suffix(".json"),row)
    print(json.dumps(row,ensure_ascii=False),flush=True)
    assert not timeout and p.returncode==0,"STOP: "+label
    return row,stdout,stderr
WATCHES={
"types.hua":[("pub fn analyze(entry string) Result<Model> {","types"),("fn infer(m mut Model, units map[string]modules.Unit) Result<void> {","return-infer")],
"modules.hua":[("pub fn analyze_compiler(entry string) Result<Graph> {","names")],
"parser.hua":[("pub fn parse_compiler(source string, name string) Result<Tree> {","parse")],
"capabilities.hua":[("pub fn analyze(graph modules.Graph, refs map[string]string, declarations map[string]string, fields map[string]string, functions map[string]Function, variants map[string]string) Result<Model> {","capabilities")],
"lowering.hua":[("pub fn lower(model types.Model) Result<Core> {","lower"),("pub fn verify(c Core) Result<void> {","core-verify")],
"bytecode.hua":[("pub fn compile(model types.Model) Result<Image> {","codegen"),("fn generate_verified(c lower.Core, model types.Model) Result<Image> {","emit")],
"bytecode_verify.hua":[("pub fn verify(image bc.Image) Result<Verification> {","bytecode-verify")],
"archive.hua":[("pub fn encode(image bc.Image, model types.Model) Result<Artifact> {","encode"),("pub fn publish(data Bytes, target string) Result<void> {","publish")]
}
def prepare(root,out,native):
    baseline=json.loads((out/"baseline-manifest.json").read_text(encoding="utf-8"))
    assert digest(native)==baseline["native"]["sha256"]
    copied=out/"instrumented-source";copied.mkdir()
    source=out/"frozen/repository/selfhost"
    for name in baseline["compiler_sources"]:shutil.copy2(source/name,copied/name)
    for name,watch in WATCHES.items():
        p=copied/name;text=p.read_text(encoding="utf-8")
        assert "_bo_phase_end" not in text
        for signature,phase in watch:
            assert text.count(signature)==1,(name,signature)
            text=text.replace(signature,signature+'\n    print("@@hua.phase:begin:'+phase+'")\n    defer _bo_phase_end("'+phase+'")\n',1)
        text+='\nfn _bo_phase_end(label string) { print("@@hua.phase:end:"+label) }\n'
        p.write_text(text,encoding="utf-8")
    component=out/"phase-component.huab"
    row,_,_=command(root,out,"prepare-phase-component",[native,"build",copied/"archive_main.hua","-o",component],90)
    save(out/"diagnostic-manifest.json",{"schema":SCHEMA,"prepared":True,"prepare":row,"instrumentation":"copied compiler sources only; begin print and deferred end at watched function bodies","phase_component_sha256":digest(component),"instrumented_sources":{p.name:digest(p) for p in copied.glob("*.hua")},"diagnostic_sources":{p.relative_to(root).as_posix():digest(p) for p in (root/"include/hua/performance.hpp",root/"src/value.cpp",root/"src/gc.cpp",root/"tests/performance_probe.cpp",root/"tests/tools/audit_bootstrap_phases.py")},"production_native_sha256":digest(native)})
def reduce_events(events):
    assert len(events)>=2
    stack=[];totals=defaultdict(lambda:{"ns":0,"cpu_ns":0,"counters":defaultdict(int),"struct_copies":defaultdict(lambda:{"objects":0,"fields":0}),"entries":0})
    previous=None
    for event in events:
        if previous is not None:
            assert event["ns"]>=previous["ns"] and event["cpu_ns"]>=previous["cpu_ns"],"clock went backwards"
            assert stack,"unparented interval"
            row=totals[stack[-1]];row["ns"]+=event["ns"]-previous["ns"];row["cpu_ns"]+=event["cpu_ns"]-previous["cpu_ns"]
            for key,value in event["counters"].items():
                if isinstance(value,int):
                    delta=value-previous["counters"][key];assert delta>=0,(key,delta)
                    row["counters"][key]+=delta
            old=previous["counters"].get("struct_copies",{})
            for name,counts in event["counters"].get("struct_copies",{}).items():
                for key in ("objects","fields"):
                    delta=counts[key]-old.get(name,{}).get(key,0);assert delta>=0
                    row["struct_copies"][name][key]+=delta
        if event["event"]=="begin":
            stack.append(event["phase"]);totals[event["phase"]]["entries"]+=1
        else:
            assert event["event"]=="end" and stack and stack[-1]==event["phase"],"unbalanced phases"
            stack.pop()
        previous=event
    assert not stack,"incomplete trace"
    assert events[0]["phase"]==events[-1]["phase"]=="execution"
    assert sum(row["ns"] for row in totals.values())==events[-1]["ns"]-events[0]["ns"]
    for key,value in events[-1]["counters"].items():
        if isinstance(value,int):assert sum(row["counters"][key] for row in totals.values())==value-events[0]["counters"][key]
    return dict(totals)
class ReducerTests(unittest.TestCase):
    def event(self,kind,phase,ns,copies):return {"event":kind,"phase":phase,"ns":ns,"cpu_ns":ns,"counters":{"copy_struct":copies,"struct_copies":{"Node":{"objects":copies,"fields":copies*3}}}}
    def events(self):return [self.event("begin","execution",0,0),self.event("begin","parse",10,1),self.event("end","parse",30,3),self.event("end","execution",50,5)]
    def test_nested_exclusive(self):
        rows=reduce_events(self.events());self.assertEqual((rows["parse"]["ns"],rows["execution"]["ns"]),(20,30));self.assertEqual(rows["parse"]["counters"]["copy_struct"],2);self.assertEqual(rows["execution"]["struct_copies"]["Node"]["fields"],9)
    def test_unclosed(self):
        with self.assertRaises(AssertionError):reduce_events(self.events()[:-1])
    def test_mismatch(self):
        events=self.events();events[2]["phase"]="encode"
        with self.assertRaises(AssertionError):reduce_events(events)
    def test_windows_newlines(self):self.assertEqual(program_text(b"15\r\n"),"15\n")
    def test_literal_carriage_return(self):self.assertEqual(program_text(b"x\ry\n"),"x\ry\n")
    def test_counter_rollback(self):
        events=self.events();events[2]["counters"]["copy_struct"]=0
        with self.assertRaises(AssertionError):reduce_events(events)
def run_cases(root,out,native,probe):
    baseline=json.loads((out/"baseline-manifest.json").read_text(encoding="utf-8"))
    assert digest(native)==baseline["native"]["sha256"]
    prepared=json.loads((out/"diagnostic-manifest.json").read_text(encoding="utf-8"))
    assert digest(out/"phase-component.huab")==prepared["phase_component_sha256"]
    probe_hash=digest(probe)
    inputs=out/"inputs";inputs.mkdir(exist_ok=True);rows=[]
    smoke=inputs/"smoke.hua";smoke.write_text("fn main(){\n"+"".join(f"let x{i}={i}\n" for i in range(16))+"print(x15)}\n",encoding="utf-8")
    cases=[("smoke",smoke,"15\n")]
    for name in ("catalog","type_rules"):
        source=inputs/(name+".hua");shutil.copy2(out/"frozen/repository/selfhost"/(name+".hua"),source);cases.append((name,source,""))
    try:
        for name,source,expected in cases:
            original=out/(name+"-original.huab");observed=out/(name+"-observed.huab")
            normal,stdout,_=command(root,out,name+"-normal",[native,"run",out/"baseline-component.huab","--execution-profile","compiler","--report-budget","--",source,original])
            receipt=json.loads(stdout);assert receipt["status"]=="ok",receipt
            control=None
            if name=="smoke":
                control,control_out,_=command(root,out,name+"-counter-control",[probe,out/"baseline-component.huab",source,out/"smoke-control.huab"])
                assert control_out==stdout and (out/"smoke-control.huab").read_bytes()==original.read_bytes()
            events=out/(name+"-phases.jsonl")
            diagnostic,observed_out,_=command(root,out,name+"-diagnostic",[probe,"--phase-events",events,out/"phase-component.huab",source,observed])
            assert observed_out==stdout and observed.read_bytes()==original.read_bytes(),"instrumentation changed output"
            phase_events=[json.loads(line) for line in events.read_text(encoding="utf-8").splitlines()]
            phases=reduce_events(phase_events)
            assert "parse" in phases and "capabilities" in phases and "publish" in phases
            _,program_out,_=command(root,out,name+"-artifact-run",[native,"run",observed])
            assert program_text(program_out)==expected
            row={"case":name,"source_sha256":digest(source),"source_bytes":source.stat().st_size,"normal":normal,"counter_control":control,"diagnostic":diagnostic,"artifact_sha256":digest(observed),"artifact_bytes":observed.stat().st_size,"exact_output_equal":True,"independent_reader_execution":True,"phases_exclusive":phases,"timing_note":"One sample per configuration; includes counter/histogram/marker overhead. No statistical speedup claim."}
            rows.append(row);save(out/"phase-results.json",{"schema":SCHEMA,"status":"running","probe_sha256":digest(probe),"rows":rows,"full_own_source_run":False})
        bad=inputs/"bad.hua";bad.write_text('fn main(){let x int="wrong"}\n',encoding="utf-8")
        _,original_error,_=command(root,out,"bad-normal",[native,"run",out/"baseline-component.huab","--execution-profile","compiler","--",bad,out/"bad-original.huab"])
        _,observed_error,_=command(root,out,"bad-diagnostic",[probe,"--phase-events",out/"bad-phases.jsonl",out/"phase-component.huab",bad,out/"bad-observed.huab"])
        assert original_error==observed_error and json.loads(original_error)["status"]=="error"
        assert not (out/"bad-original.huab").exists() and not (out/"bad-observed.huab").exists()
        reduce_events([json.loads(line) for line in (out/"bad-phases.jsonl").read_text(encoding="utf-8").splitlines()])
        assert digest(probe)==probe_hash and digest(native)==baseline["native"]["sha256"],"binary changed during audit"
        assert {name:digest(p) for name,p in closure(root/"selfhost/archive_main.hua").items()}==baseline["compiler_sources"],"compiler sources changed"
        result={"schema":SCHEMA,"status":"passed","probe_sha256":digest(probe),"rows":rows,"error_output_equal":True,"error_trace_balanced":True,"full_own_source_run":False,"native_stack_sampling":False}
        save(out/"phase-results.json",result)
    except BaseException as error:
        save(out/"phase-results.json",{"schema":SCHEMA,"status":"stopped","error":str(error),"rows":rows,"full_own_source_run":False})
        raise
    print(json.dumps({"status":"passed","cases":len(rows),"error_equivalence":True,"full_own_source_run":False}))
def main():
    global RESUME
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("self-test","freeze","prepare","run"))
    parser.add_argument("--root",type=Path,default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output",type=Path)
    parser.add_argument("--native",type=Path)
    parser.add_argument("--probe",type=Path)
    parser.add_argument("--resume",action="store_true",help="Reuse only ledger-verified completed commands after a harness-only fix.")
    args=parser.parse_args()
    if args.mode=="self-test":
        result=unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(ReducerTests))
        return 0 if result.wasSuccessful() else 1
    assert args.output and args.native
    root=args.root.resolve();out=args.output.resolve();native=args.native.resolve()
    assert out.is_relative_to(root/"tests/artifacts") and out!=root/"tests/artifacts","evidence must have a dedicated tests/artifacts subdirectory"
    if args.mode=="freeze":freeze(root,out,native)
    elif args.mode=="prepare":prepare(root,out,native)
    else:
        assert args.probe
        if args.resume:
            RESUME=json.loads((out/"harness-revision.json").read_text(encoding="utf-8"))
            assert digest(native)==RESUME["native_sha256"] and digest(args.probe)==RESUME["probe_sha256"]
            for rel,sha in RESUME["verified_files"].items():assert digest(out/rel)==sha,rel
        run_cases(root,out,native,args.probe.resolve())
    return 0
if __name__=="__main__":raise SystemExit(main())
