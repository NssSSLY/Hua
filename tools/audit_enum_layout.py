"""BO-04 same-artifact runtime comparison; consumes frozen BO-01/02 evidence."""
from pathlib import Path
import argparse, json, os
from audit_bootstrap_phases import command, digest, save, reduce_events, closure
from audit_enum_kernel import SIZES, OPS, ROUNDS, oracle, parse_output, probe_args
def identities(root):
    return {"native":digest(root/"build/hua.exe"),"probe":digest(root/"build-a11-profile/hua_performance_probe.exe")}
def kernels(root,out):
    assert not out.exists(),"choose a fresh evidence directory";out.mkdir(parents=True)
    prior=root/"build/bo02-enum-20261016-r2"
    before=json.loads((prior/"results.json").read_text(encoding="utf-8"))
    manifest=json.loads((prior/"manifest.json").read_text(encoding="utf-8"))
    native=root/"build/hua.exe";probe=root/"build-a11-profile/hua_performance_probe.exe"
    frozen=identities(root);data={"status":"running","before_evidence":str(prior),"identities":frozen,"rows":[],"full_own_source_run":False}
    save(out/"identities.json",{"current":frozen,"baseline":manifest["identities"],"kernel_rounds":ROUNDS})
    try:
        for n in SIZES:
            for packed in (False,True):
                representation="hua-packed" if packed else "hua-enum"
                name=("packed" if packed else "enum")+f"-{n}"
                artifact=prior/(name+".huab")
                previous={r["operation"]:r for r in before["rows"] if r["variants"]==n and r["representation"]==representation}
                assert digest(artifact)==previous["copy"]["artifact_sha256"]
                normal,stdout,_=command(root,out,name+"-normal",[native,"run",artifact,"--execution-profile","compiler","--report-budget"])
                logical,shape=parse_output(stdout);events=out/(name+"-events.jsonl")
                diagnostic,observed,stderr=command(root,out,name+"-counts",probe_args(probe,events,artifact))
                counted,counted_shape=parse_output(observed);assert shape==counted_shape==previous["copy"]["shape_observation"]
                trace=[json.loads(line) for line in events.read_text(encoding="utf-8").splitlines()]
                phases=reduce_events(trace);assert json.loads(stderr.decode("utf-8").splitlines()[0])==trace[-1]["counters"]
                for op in OPS:
                    expected,state=oracle(n,op,ROUNDS);old=previous[op];counts=phases[op]["counters"]
                    assert (logical[op]["checksum"],logical[op]["state"])==(expected,state)
                    assert (counted[op]["checksum"],counted[op]["state"])==(expected,state)
                    assert counts["copy_struct_fields"]==2*counts["copy_struct"]
                    assert counts["copy_struct"]==old["counts"]["copy_struct"]
                    assert counts["vm_steps"]==old["counts"]["vm_steps"]
                    assert logical[op]["managed_allocations"]==counted[op]["managed_allocations"]==old["kernel"]["managed_allocations"]
                    assert counts["gc_collections"]==0
                    data["rows"].append({"representation":representation,"variants":n,"operation":op,"artifact_sha256":digest(artifact),"before":old["kernel"],"after":logical[op],"before_counts":old["counts"],"after_counts":counts,"normal_process":normal,"diagnostic_process":diagnostic,"shape":shape})
                save(out/"results.json",data)
        assert identities(root)==frozen
        data["status"]="passed";save(out/"results.json",data)
    except BaseException as failure:data["status"]="stopped";data["error"]=repr(failure);save(out/"results.json",data);raise
    print(json.dumps({"status":"passed","kernel_rows":len(data["rows"]),"same_artifact":True}))
def real(root,out):
    assert not out.exists(),"choose fresh evidence";out.mkdir(parents=True)
    prior=root/"build/bo00-01-20261016";previous=json.loads((prior/"phase-results.json").read_text(encoding="utf-8"))
    baseline=json.loads((prior/"baseline-manifest.json").read_text(encoding="utf-8"))
    prepared=json.loads((prior/"diagnostic-manifest.json").read_text(encoding="utf-8"))
    compiler=prior/"baseline-component.huab";instrumented=prior/"phase-component.huab"
    assert digest(compiler)==baseline["baseline_component"]["sha256"]
    assert digest(instrumented)==prepared["phase_component_sha256"]
    assert {name:digest(path) for name,path in closure(root/"selfhost/archive_main.hua").items()}==baseline["compiler_sources"]
    native=root/"build/hua.exe";probe=root/"build-a11-profile/hua_performance_probe.exe";frozen=identities(root)
    data={"status":"running","identities":frozen,"component_sha256":digest(compiler),"instrumented_sha256":digest(instrumented),"compiler_sources":baseline["compiler_sources"],"rows":[],"full_own_source_run":False}
    try:
        for name in ("catalog","type_rules"):
            old=next(r for r in previous["rows"] if r["case"]==name)
            source=prior/"inputs"/(name+".hua");assert digest(source)==old["source_sha256"]
            artifact=out/(name+"-normal.huab");observed=out/(name+"-observed.huab")
            normal,stdout,_=command(root,out,name+"-normal",[native,"run",compiler,"--execution-profile","compiler","--report-budget","--",source,artifact])
            assert stdout==(prior/"commands"/(name+"-normal.stdout")).read_bytes(),"full compiler report differs"
            expected=(prior/(name+"-original.huab")).read_bytes()
            assert artifact.read_bytes()==expected,"full generated HUAB bytes differ"
            events=out/(name+"-phases.jsonl")
            diagnostic,diagout,stderr=command(root,out,name+"-diagnostic",[probe,"--phase-events",events,instrumented,source,observed])
            assert diagout==stdout and observed.read_bytes()==expected
            trace=[json.loads(line) for line in events.read_text(encoding="utf-8").splitlines()]
            phases=reduce_events(trace);counts=trace[-1]["counters"]
            assert counts==json.loads(stderr.decode("utf-8").splitlines()[0])
            oldtrace=[json.loads(line) for line in (prior/(name+"-phases.jsonl")).read_text(encoding="utf-8").splitlines()]
            oldcounts=oldtrace[-1]["counters"]
            assert counts["vm_steps"]==oldcounts["vm_steps"]
            assert counts["copy_struct"]==oldcounts["copy_struct"]
            payloads={key:value for key,value in counts["struct_copies"].items() if key.endswith("$Payload") or key=="Payload"}
            assert len(payloads)==1,payloads
            key,payload=next(iter(payloads.items()))
            assert payload["fields"]==payload["objects"]*2
            assert oldcounts["struct_copies"][key]["objects"]==payload["objects"]
            assert oldcounts["struct_copies"][key]["fields"]==payload["objects"]*60
            assert oldcounts["copy_struct_fields"]-counts["copy_struct_fields"]==payload["objects"]*58
            _,program,_=command(root,out,name+"-artifact-run",[native,"run",observed]);assert not program
            data["rows"].append({"case":name,"source_sha256":digest(source),"source_bytes":source.stat().st_size,"artifact_sha256":digest(observed),"exact_report_and_artifact_equal":True,"normal_before":old["normal"],"normal_after":normal,"diagnostic_after":diagnostic,"counts_before":oldcounts,"counts_after":counts,"phases_before":old["phases_exclusive"],"phases_after":phases,"execution_seconds_before":(oldtrace[-1]["ns"]-oldtrace[0]["ns"])/1e9,"execution_seconds_after":(trace[-1]["ns"]-trace[0]["ns"])/1e9})
            save(out/"results.json",data)
        source=prior/"inputs/bad.hua"
        _,stdout,_=command(root,out,"bad-normal",[native,"run",compiler,"--execution-profile","compiler","--",source,out/"bad-normal.huab"])
        _,observed,_=command(root,out,"bad-diagnostic",[probe,"--phase-events",out/"bad-events.jsonl",instrumented,source,out/"bad-observed.huab"])
        assert stdout==observed==(prior/"commands/bad-normal.stdout").read_bytes()
        assert not (out/"bad-normal.huab").exists() and not (out/"bad-observed.huab").exists()
        reduce_events([json.loads(line) for line in (out/"bad-events.jsonl").read_text(encoding="utf-8").splitlines()])
        assert identities(root)==frozen
        assert {name:digest(path) for name,path in closure(root/"selfhost/archive_main.hua").items()}==baseline["compiler_sources"]
        data["status"]="passed";data["error_report_and_trace_equal"]=True;save(out/"results.json",data)
    except BaseException as failure:data["status"]="stopped";data["error"]=repr(failure);save(out/"results.json",data);raise
    print(json.dumps({"status":"passed","real_inputs":len(data["rows"]),"full_report_and_artifact_equal":True,"full_own_source_run":False}))
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("mode",choices=("kernels","real"));p.add_argument("--root",type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument("--output",type=Path,required=True)
    args=p.parse_args();root=args.root.resolve();out=args.output.resolve()
    assert out.is_relative_to(root/"build") and out!=root/"build"
    os.environ["PATH"]=str(root/"build")+os.pathsep+os.environ.get("PATH","")
    (kernels if args.mode=="kernels" else real)(root,out)
if __name__=="__main__":main()
