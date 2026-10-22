"""BO-02 enum construct/copy/match kernels. No production representation changes."""
from pathlib import Path
import argparse, json, os, shutil, subprocess, unittest
import audit_bootstrap_phases as base
from audit_bootstrap_phases import command, digest, save, reduce_events, machine
SIZES=(1,8,32,59)
OPS=("construct","copy","match-first","match-last")
ROUNDS=1024
def oracle(n,op,rounds):
    state=17
    if op=="construct":
        for _ in range(rounds):state=state*48271%2147483647
        checksum=state+n-1
    elif op=="copy":checksum=17+n-1
    else:checksum=rounds*(17+(0 if op=="match-first" else n-1))
    return checksum,state
def source(n,compact):
    imports="import std.time as time\nimport std.gc as gc\nimport std.json as json\n"
    if compact:
        declaration="struct Item{tag int\nvalue int}\n"
        constructors=f"fn make_first(value int) Item{{return Item{{tag:0,value:value}}}}\nfn make_last(value int) Item{{return Item{{tag:{n-1},value:value}}}}\n"
        reader="fn read(item Item) int{\n"+"".join(f"if item.tag=={k}{{return item.value+{k}}}\n" for k in range(n))+"return -1}\n"
        first,last="make_first","make_last"
    else:
        declaration="enum Item{"+",".join(f"V{k}(int)" for k in range(n))+"}\n";constructors=""
        reader="fn read(item Item) int{match item{\n"+"".join(f"V{k}(value)=>{{return value+{k}}}\n" for k in range(n))+"}}\n"
        first,last="Item.V0",f"Item.V{n-1}"
    functions=[]
    for op in OPS:
        name=op.replace("-","_")
        make=first if op=="match-first" else last
        body=f"fn kernel_{name}(){{\nvar held={make}(17)\nlet original=held\nvar state=17\nvar checksum=0\nlet before=gc.allocated()\n"
        body+=f'print("@@hua.phase:begin:{op}")\nlet start=time.monotonic_ns()\nfor i in 0..{ROUNDS}{{\n'
        if op=="construct":body+=f"state=state*48271%2147483647\nheld={last}(state)\n"
        elif op=="copy":body+="let copied=original\nheld=copied\n"
        else:body+="checksum+=read(original)\n"
        body+='}\nlet elapsed=time.monotonic_ns()-start\n'
        body+=f'print("@@hua.phase:end:{op}")\nlet allocated=gc.allocated()-before\n'
        if op in ("construct","copy"):body+="checksum=read(held)\n"
        body+=f'print("RESULT","{op}",checksum,elapsed,allocated,state)\n}}\n'
        functions.append(body)
    main="fn main(){\n"+"".join("kernel_"+op.replace("-","_")+"()\n" for op in OPS)
    main+=f'print("SHAPE",is_err(json.encode({last}(17))),str({last}(17)))\n}}\n'
    return imports+declaration+constructors+reader+"".join(functions)+main
def parse_output(stdout):
    rows={};shape=None
    for line in stdout.decode("utf-8").splitlines():
        if line.startswith("RESULT "):
            fields=line.split();assert len(fields)==6,line
            _,op,checksum,ns,allocated,state=fields
            assert op in OPS and op not in rows
            rows[op]={"checksum":int(checksum),"elapsed_ns":int(ns),"managed_allocations":int(allocated),"state":int(state)}
        elif line.startswith("SHAPE "):
            _,rejected,display=line.split(" ",2);shape={"json_rejected":rejected=="true","display":display}
        else:assert line.startswith("@@hua.phase:"),line
    assert set(rows)==set(OPS) and shape is not None
    return rows,shape
def probe_args(probe,events,artifact):
    # The shared compiler probe requires at least one argument after the component.
    # These kernels never inspect argv; the extra argument has no program effect.
    return [probe,"--phase-events",events,artifact,"bo02-kernel"]
def resume_verified(root,out):
    ledger=json.loads((out/"resume-ledger.json").read_text(encoding="utf-8"))
    assert all(digest(Path(path))==sha for path,sha in ledger["files"].items()),"resume evidence changed"
    manifest=json.loads((out/"manifest.json").read_text(encoding="utf-8"))
    assert all(digest(root/rel)==sha for rel,sha in manifest["files"].items() if rel!=ledger["allowed_source_revision"]),"frozen repository changed"
    assert digest(Path(__file__))==ledger["revised_source_sha256"],"unexpected harness revision"
    assert all(digest(Path(v["path"]))==v["sha256"] for v in manifest["identities"].values()),"runtime identity changed"
    base.RESUME=ledger
class Tests(unittest.TestCase):
    def test_probe_arguments(self):
        args=probe_args("probe","events","component")
        self.assertEqual(args,["probe","--phase-events","events","component","bo02-kernel"])

    def test_oracle_fixed(self):
        self.assertEqual(oracle(8,"copy",1024),(24,17));self.assertEqual(oracle(8,"match-last",1024),(24576,17));self.assertEqual(oracle(8,"match-first",1024),(17408,17))
    def test_oracle_construct(self):self.assertEqual(oracle(8,"construct",1),(820614,820607))
    def test_generated_shape(self):
        full=source(59,False);packed=source(59,True)
        self.assertEqual(full.count("(int)"),59);self.assertIn("Item.V58(state)",full);self.assertIn("tag:58",packed)
        self.assertEqual(full.count('print("@@hua.phase:begin:'),4);self.assertEqual(packed.count('print("@@hua.phase:end:'),4)
    def test_windows_output(self):
        data=b"".join(f"RESULT {op} 1 2 3 17\r\n".encode() for op in OPS)+b"SHAPE true Item.V0(17)\r\n"
        rows,shape=parse_output(data);self.assertEqual(rows["copy"]["elapsed_ns"],2);self.assertTrue(shape["json_rejected"])
    def test_incomplete_output(self):
        with self.assertRaises(AssertionError):parse_output(b"RESULT copy 1 2 3 17\n")
def freeze(root,out,native,probe):
    assert not out.exists(),"choose a fresh evidence directory"
    out.mkdir(parents=True)
    names=subprocess.check_output(["git","ls-files","-z","--cached","--others","--exclude-standard"],cwd=root).split(b"\0")
    files={rel.decode("utf-8"):digest(root/rel.decode("utf-8")) for rel in names if rel and (root/rel.decode("utf-8")).is_file()}
    prior=json.loads((root/"tests/artifacts/build/bo00-01-20261016/bo00-01-summary.json").read_text(encoding="utf-8"))
    assert digest(native)==prior["native_sha256"] and digest(probe)==prior["probe_sha256"]
    paths={"native":native,"probe":probe,"semantics":root/"tests/artifacts/build/libhua_semantics.a","frontend":root/"tests/artifacts/build/libhua_frontend.a","wasmtime":root/"tests/artifacts/build/wasmtime.dll","clang":root/".tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe"}
    save(out/"manifest.json",{"schema":"hua.enum.kernel.v1","files":files,"host":machine(),"identities":{k:{"path":str(p),"sha256":digest(p)} for k,p in paths.items()},"git_head":subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip(),"sizes":SIZES,"rounds":ROUNDS,"full_own_source_run":False})
    for rel in ("tests/enum_kernel_probe.cpp","tests/tools/audit_enum_kernel.py","selfhost/ast_pool.hua","src/lowering.cpp","selfhost/lowering.hua","src/value.cpp","src/bytecode.cpp","src/json.cpp","docs/语言设计与类型规则.md"):
        p=out/"frozen"/rel;p.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/rel,p)
def run(root,out,native,probe,resume=False):
    if resume:resume_verified(root,out)
    else:freeze(root,out,native,probe)
    # This task-scoped DLL search path only affects child processes.
    os.environ["PATH"]=str(root/"tests/artifacts/build")+os.pathsep+os.environ.get("PATH","")
    clang=root/".tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe"
    executable=out/"enum-kernel.exe"
    compile_args=[clang,"-std=c++20","-O3","-DNDEBUG","-static","-Wl,--stack,8388608","-I",root/"include",root/"tests/enum_kernel_probe.cpp",root/"tests/artifacts/build/libhua_semantics.a",root/"tests/artifacts/build/libhua_frontend.a","-lpthread",root/".tools/wasmtime-v49.0.2-x86_64-windows-c-api/lib/wasmtime.dll.lib",*("-l"+name for name in ("kernel32","user32","gdi32","winspool","shell32","ole32","oleaut32","uuid","comdlg32","advapi32")),"-o",executable]
    data={"schema":"hua.enum.kernel.v1","status":"running","rounds":ROUNDS,"sizes":SIZES,"rows":[],"native":[],"full_own_source_run":False}
    try:
        command(root,out,"native-build",compile_args,90)
        _,stdout,_=command(root,out,"native-preflight",[executable,"--preflight"])
        preflight=json.loads(stdout);assert preflight["status"]=="passed";data["native_preflight"]=preflight
        for n in SIZES:
            for compact in (False,True):
                name=("packed" if compact else "enum")+f"-{n}"
                hua=out/(name+".hua");artifact=out/(name+".huab");hua.write_bytes(source(n,compact).encode("utf-8"))
                build,_,_=command(root,out,name+"-build",[native,"build",hua,"-o",artifact])
                normal,outbytes,_=command(root,out,name+"-normal",[native,"run",artifact,"--execution-profile","compiler","--report-budget"])
                logical,shape=parse_output(outbytes)
                events=out/(name+"-events-v2.jsonl")
                diagnostic,diagbytes,stderr=command(root,out,name+"-counts-v2",probe_args(probe,events,artifact))
                diagnostic_logical,diagnostic_shape=parse_output(diagbytes);assert shape==diagnostic_shape
                trace=[json.loads(line) for line in events.read_text(encoding="utf-8").splitlines()];phases=reduce_events(trace)
                terminal=json.loads(stderr.decode("utf-8").splitlines()[0]);assert terminal==trace[-1]["counters"]
                for op in OPS:
                    checksum,state=oracle(n,op,ROUNDS)
                    for result in (logical[op],diagnostic_logical[op]):
                        assert (result["checksum"],result["state"])==(checksum,state),(name,op,result,(checksum,state))
                    assert logical[op]["managed_allocations"]==diagnostic_logical[op]["managed_allocations"]
                    phase=phases[op];counts=phase["counters"]
                    assert counts["copy_struct"]>0
                    assert counts["copy_struct_fields"]==counts["copy_struct"]*(2 if compact else n+1)
                    data["rows"].append({"representation":"hua-packed" if compact else "hua-enum","variants":n,"operation":op,"rounds":ROUNDS,"source_sha256":digest(hua),"artifact_sha256":digest(artifact),"artifact_bytes":artifact.stat().st_size,"build_seconds":build["seconds"],"kernel":logical[op],"normal_process":normal,"diagnostic_process":diagnostic,"counts":counts,"copy_histogram":phase["struct_copies"],"shape_observation":shape})
                assert shape["json_rejected"]== (not compact)
                save(out/"results.json",data)
            _,stdout,_=command(root,out,f"native-{n}",[executable,str(n),str(ROUNDS)])
            native_rows=[json.loads(line) for line in stdout.decode("utf-8").splitlines()]
            assert len(native_rows)==12
            for row in native_rows:
                expected,state=oracle(n,row["operation"],ROUNDS)
                assert row["checksum"]==expected*row["batches"] and row["state"]==state,row
                assert row["operations"]==ROUNDS*row["batches"]
                if row["representation"]!="std-variant":assert row["managed_allocations"]==2*row["operations"]
            data["native"].extend(native_rows);save(out/"results.json",data)
        manifest=json.loads((out/"manifest.json").read_text(encoding="utf-8"))
        assert all(digest(Path(v["path"]))==v["sha256"] for v in manifest["identities"].values())
        data["status"]="passed";data["native_executable_sha256"]=digest(executable)
        save(out/"results.json",data)
    except BaseException as error:
        data["status"]="stopped";data["error"]=repr(error);save(out/"results.json",data);raise
    print(json.dumps({"status":data["status"],"hua_rows":len(data["rows"]),"native_rows":len(data["native"]),"full_own_source_run":False}))
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("mode",choices=("self-test","run"));p.add_argument("--root",type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument("--output",type=Path);p.add_argument("--resume",action="store_true",help="reuse only hash-verified commands from resume-ledger.json")
    args=p.parse_args()
    if args.mode=="self-test":return 0 if unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests)).wasSuccessful() else 1
    root=args.root.resolve();out=args.output.resolve()
    assert out.is_relative_to(root/"tests/artifacts") and out!=root/"tests/artifacts"
    run(root,out,root/"tests/artifacts/build/hua.exe",root/"tests/artifacts/build-a11-profile/hua_performance_probe.exe",args.resume);return 0
if __name__=="__main__":raise SystemExit(main())
