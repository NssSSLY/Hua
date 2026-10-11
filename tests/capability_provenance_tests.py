"""A07-1: readonly descendants, legal record copies, four analyzer paths and HUAB."""
from pathlib import Path
import json,subprocess,sys,shutil,re
hua,probe,repo,build=map(lambda x:Path(x).resolve(),sys.argv[1:])
driver=build/("hua_instruction_driver"+hua.suffix)
out=build/"a07";out.mkdir(exist_ok=True);cases=out/"cases";cases.mkdir(exist_ok=True)
rows=[];checks=0
def call(args,code=0):
    global checks
    p=subprocess.run(list(map(str,args)),capture_output=True,encoding="utf-8",timeout=600)
    assert p.returncode==code,(args,p.returncode,p.stdout[:1200],p.stderr[:2000])
    checks+=1
    return p
header="struct Box{xs []int\nx int}\n"
fresh="Box{xs:[1],x:1}"
positive={
"optional_scalar_copy":(header+"fn main(){let ro Box?="+fresh+"\nvar b=ro\nif b!=nil{b.x=2\nprint(b.x)}}\n","2\n"),
"scalar_copy":(header+"fn main(){let r Result<Box>=ok("+fresh+")\nvar b=unwrap(r)\nb.x=2\nprint(b.x)}\n","2\n"),
"field_refresh":(header+"fn main(){let r Result<Box>=ok("+fresh+")\nvar b=unwrap(r)\nb.xs=[3]\nb.xs[0]=4\nprint(b.xs)}\n","[4]\n"),
"fresh_result":(header+"fn make() Result<Box>{return ok("+fresh+")}\nfn main(){var r=make()\nvar b=unwrap(r)\nb.xs[0]=2\nprint(b.xs)}\n","[2]\n"),
"alias_refresh":(header+"fn main(){let ro="+fresh+"\nvar b=ro\nb.xs=[3]\nvar c=b\nc.xs[0]=4\nprint(c.xs)}\n","[4]\n"),
"list_record_scalar":("import std.list as l\n"+header+"fn main(){var xs=l.from_slice(["+fresh+"])\nvar b=unwrap(l.get(xs,0))\nb.x=2\nprint(b.x)}\n","2\n"),
"nested_refresh":(header+"struct Outer{b Box}\nfn main(){let ro=Outer{b:"+fresh+"}\nvar o=ro\no.b="+fresh+"\no.b.xs[0]=3\nprint(o.b.xs)}\n","[3]\n"),
"clone_record":(header+"fn main(){let ro="+fresh+"\nvar b=clone(ro)\nb.xs[0]=2\nprint(b.xs,ro.xs)}\n","[2] [1]\n"),
"multi_refresh":(header+"fn main(){let ro="+fresh+"\nvar b,k=ro,0\nb.xs=[2]\nb.xs[0]=3\nprint(b.xs,k)}\n","[3] 0\n"),
}
negative={
"optional_record":header+"print(\"BAD\")\nfn main(){let ro Box?="+fresh+"\nvar b=ro\nif b!=nil{b.xs[0]=2}}\n",
"optional_wrapped":header+"print(\"BAD\")\nfn main(){let ro Box?="+fresh+"\nvar r Result<Box?>=ok(ro)\nvar b=unwrap(unwrap(r))\nb.xs[0]=2}\n",
"plain_record":header+"print(\"BAD\")\nfn main(){let ro="+fresh+"\nvar b=ro\nb.xs[0]=2}\n",
"result_record":header+"print(\"BAD\")\nfn main(){let r Result<Box>=ok("+fresh+")\nvar b=unwrap(r)\nb.xs[0]=2}\n",
"function_record":header+"fn get(r Result<Box>) Box{return unwrap(r)}\nprint(\"BAD\")\nfn main(){let r Result<Box>=ok("+fresh+")\nvar b=get(r)\nb.xs[0]=2}\n",
"list_record":"import std.list as l\n"+header+"print(\"BAD\")\nfn main(){let xs=l.from_slice(["+fresh+"])\nvar b=unwrap(l.get(xs,0))\nb.xs[0]=2}\n",
"map_optional":header+"print(\"BAD\")\nfn main(){let m=map[string]Box{\"a\":"+fresh+"}\nvar b=unwrap(m[\"a\"])\nb.xs[0]=2}\n",
"error_payload":header+"print(\"BAD\")\nfn main(){let r Result<int,Box>=err("+fresh+")\nvar b=unwrap_err(r)\nb.xs[0]=2}\n",
"propagated":header+"print(\"BAD\")\nfn change() Result<void>{let r Result<Box>=ok("+fresh+")\nvar b=r?\nb.xs[0]=2\nreturn ok()}\nfn main(){unwrap(change())}\n",
"shared_alias":header+"print(\"BAD\")\nfn main(){let ro="+fresh+"\nvar b=ro\nvar xs=b.xs\nxs[0]=2}\n",
"mut_child":header+"fn change(xs mut []int){xs[0]=2}\nprint(\"BAD\")\nfn main(){let ro="+fresh+"\nvar b=ro\nchange(b.xs)}\n",
"nested_result":header+"print(\"BAD\")\nfn main(){let r Result<Result<Box>>=ok(ok("+fresh+"))\nvar b=unwrap(unwrap(r))\nb.xs[0]=2}\n",
"reset_override":header+"print(\"BAD\")\nfn main(){let ro="+fresh+"\nvar b=ro\nb.xs=[3]\nb=ro\nb.xs[0]=2}\n",
"branch_refresh":header+"print(\"BAD\")\nfn main(){let ro="+fresh+"\nvar b=ro\nif true{b.xs=[3]}\nb.xs[0]=2}\n",
"branch_downgrade":header+"print(\"BAD\")\nfn main(){let ro=[1]\nvar b="+fresh+"\nif true{b.xs=ro}\nb.xs[0]=2}\n",
"loop_refresh":header+"print(\"BAD\")\nfn main(){let ro="+fresh+"\nvar b=ro\nwhile false{b.xs=[3]}\nb.xs[0]=2}\n",
"multi_record":header+"print(\"BAD\")\nfn main(){let ro="+fresh+"\nvar b,k=ro,0\nb.xs[0]=2}\n",
"multi_reassign":header+"print(\"BAD\")\nfn main(){let ro="+fresh+"\nvar b="+fresh+"\nvar k=0\nb,k=ro,1\nb.xs[0]=2}\n",
}
persisted=out/"analyzer";persisted.mkdir(exist_ok=True)
for f in (repo/"selfhost").glob("*.hua"):shutil.copy2(f,persisted/f.name)
artifact=persisted/"types.huab";call([probe,"build",persisted/"types_main.hua","-o",artifact])
assert persisted.parent==out
for f in persisted.glob("*.hua"):f.unlink()
assert not list(persisted.glob("*.hua"))
paths=[(hua,"interpret",repo/"selfhost/types_main.hua"),(hua,"run",repo/"selfhost/types_main.hua"),(hua,"run",artifact),(probe,"run",repo/"selfhost/types_main.hua")]
for phase,fixtures in [("positive",positive),("negative",negative)]:
    for name,data in fixtures.items():
        text,wanted=data if phase=="positive" else (data,"")
        d=cases/name;d.mkdir(exist_ok=True);source=d/"input.hua";source.write_text(text,encoding="utf-8",newline="\n")
        source_artifact=d/"input.huab";source_artifact.unlink(missing_ok=True)
        executors=[hua] if name=="error_payload" else [hua,probe]
        for exe in executors:
            for action in ["check","interpret","run","build"]:
                args=[exe,action,source]
                if action=="build":args+=["-o",source_artifact]
                result=call(args,0 if phase=="positive" else 1)
                if phase=="negative":
                    assert "[E3003]" in result.stderr and not result.stdout,(name,result)
                    assert not source_artifact.exists()
                elif action in ["interpret","run"]:assert result.stdout==wanted,(name,result.stdout,wanted)
        if name=="error_payload":
            rows.append({"case":name,"phase":phase,"paths":"production AST/VM; SB1 and C04 exclude non-string error"})
            print("A07",name,"production passed",flush=True)
            continue
        reports=[]
        for exe,action,entry in paths:
            result=call([exe,action,entry,"--execution-profile","compiler","--",source])
            reports.append(json.loads(result.stdout))
        assert all(x==reports[0] for x in reports),(name,reports)
        assert reports[0]["status"]==("ok" if phase=="positive" else "error"),(name,reports[0])
        if phase=="negative":assert re.match(r"E3003@\d+:\d+",reports[0]["error"]),(name,reports[0])
        emitted=call([hua,"run",repo/"selfhost/bytecode_main.hua","--execution-profile","compiler","--",source])
        image=json.loads(emitted.stdout)
        assert image["status"]==("ok" if phase=="positive" else "error"),(name,image)
        if phase=="negative":assert re.match(r"E3003@\d+:\d+",image["error"]),(name,image)
        else:
            instructions=d/"instructions.json";instructions.write_text(emitted.stdout,encoding="utf-8")
            assert call([driver,"run",instructions]).stdout==wanted
            generated=d/"instructions.huab";call([driver,"build",instructions,"-o",generated])
            source.unlink()
            result=call([hua,"run",source_artifact]);assert result.stdout==wanted
            assert call([hua,"run",generated]).stdout==wanted
        checks+=4;rows.append({"case":name,"phase":phase,"paths":"native AST/VM/SB1/source-free HUAB; Hua analyzer AST/VM/SB1/source-free HUAB; Hua instructions VM/source-free C++ adapter HUAB"})
        print("A07",name,"passed",flush=True)
# Controlled compile must preserve the public verifier for caller-supplied Core.
guard=out/"generation_guard";guard.mkdir(exist_ok=True)
for f in (repo/"selfhost").glob("*.hua"):shutil.copy2(f,guard/f.name)
guard.joinpath("guard.hua").write_text("""
import bytecode as b
import bytecode_report as report
import bytecode_verify as v
import lowering as l
import types as t
import std.os as os
import std.list as list
import std.strings as strings
fn main(){
    let m=unwrap(t.analyze(os.args()[0]))
    var c=unwrap(l.lower(m))
    let a=unwrap(b.generate(c,m))
    let d=unwrap(b.compile(m))
    print("equivalent",unwrap(report.success(a,m,"input.hua",unwrap(v.verify(a))))==unwrap(report.success(d,m,"input.hua",unwrap(v.verify(d)))))
    var broken=unwrap(list.get(c.nodes,0))
    broken.id=-1
    unwrap(list.set(c.nodes,0,broken))
    print("guard",strings.starts_with(unwrap_err(b.generate(c,m)),"E8203@"))
}
""",encoding="utf-8",newline="\n")
small=out/"guard_input.hua";small.write_text('fn main(){var n=0\nwhile n<3{n+=1}\nprint(n)}\n',encoding="utf-8",newline="\n")
artifact=guard/"guard.huab";call([probe,"build",guard/"guard.hua","-o",artifact])
for exe,action in [(hua,"interpret"),(hua,"run"),(probe,"run")]:
    assert call([exe,action,guard/"guard.hua","--execution-profile","compiler","--",small]).stdout=="equivalent true\nguard true\n"
assert guard.parent==out
for f in guard.glob("*.hua"):f.unlink()
assert call([hua,"run",artifact,"--execution-profile","compiler","--",small]).stdout=="equivalent true\nguard true\n"
checks+=4
(out/"provenance-results.json").write_text(json.dumps({"cases":len(rows),"checks":checks,"rows":rows},ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print("A07-1",len(rows),"cases /",checks,"checks",flush=True)
