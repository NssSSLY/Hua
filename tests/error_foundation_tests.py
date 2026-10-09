"""Independent interpreter/VM/source-free archive proof for ER1."""
from pathlib import Path
import json, struct, subprocess, sys, tempfile
exe, root, build = (Path(x).resolve() for x in sys.argv[1:])
checks=0;cases=0
def run(command,path,expected=None,error=None):
    global checks
    result=subprocess.run([str(exe),command,str(path)],capture_output=True,timeout=25)
    output=result.stdout.decode("utf-8").replace("\r\n","\n")
    stderr=result.stderr.decode("utf-8")
    if error:
        assert result.returncode==1 and error in stderr,(command,path,result.returncode,output,stderr,error)
    else:
        assert result.returncode==0,(command,path,output,stderr)
        if expected is not None:assert output==expected,(command,path,output,expected)
    checks+=1
with tempfile.TemporaryDirectory(prefix="hua-er1-",dir=build) as temporary:
    directory=Path(temporary)
    def fixture(text):
        global cases
        cases+=1;p=directory/f"case{cases}.hua";p.write_text(text,encoding="utf-8",newline="\n");return p
    def compare(text,expected,version=7):
        p=fixture(text);run("check",p,"OK\n");run("build",p)
        artifact=p.with_suffix(".huab");assert struct.unpack_from("<I",artifact.read_bytes(),8)[0]==version
        for command in ("interpret","run"):run(command,p,expected)
        p.unlink();run("check",artifact,"OK\n");run("run",artifact,expected);return artifact
    def reject(text,code):
        p=fixture(text)
        for command in ("check","interpret","run","build"):run(command,p,error=code)
        assert not p.with_suffix(".huab").exists()
    compare('fn ready() Result<void> {return ok()}\nlet r Result<void>=ok()\nprint(r,ready(),is_ok(r),is_err(r))\nunwrap(r)\n',"ok() ok() true false\n")
    compare('fn fail() Result<void> {return err("bad")}\nfn forward() Result<void> {defer print("cleanup")\nfail()?\nprint("unreachable")\nreturn ok()}\nprint(forward(),unwrap_err(fail()))\n',"cleanup\nerr(bad) bad\n")
    compare('fn ready() Result<void> {return ok()}\nfn forward() Result<void> {ready()?\nreturn ok()}\nprint(forward())\n',"ok()\n")
    compare('fn consume(r Result<void>) bool {return is_ok(r)}\nprint(consume(ok()))\n',"true\n")
    compare('async fn ready() Result<void> {return ok()}\nfn main() {let t=ready()\nprint(await t,await t)}\n',"ok() ok()\n")
    compare('async fn fail() Result<void> {return err("bad")}\nfn main() {let t=fail()\nprint(await t,await t)}\n',"err(bad) err(bad)\n")
    compare('fn ordinary() Result<int> {return ok(1)}\nprint(ordinary(),ok(nil))\n',"ok(1) ok(nil)\n",6)
    for text in ('ok()\n','let r=ok()\n','let r Result<int>=ok()\n','let r Result<void>=ok(nil)\n',
                 'fn bad() Result<int,void> {return ok(1)}\n','let r Result<void>=ok()\nlet v=unwrap(r)\n',
                 'fn bad() Result<void> {let r Result<void>=ok()\nlet v=r?\nreturn ok()}\n',
                 'let r Result<void>=ok()\nprint(unwrap(r))\n','let r Result<void>=ok()\nunwrap_or(r,nil)\n',
                 'let r Result<void>=ok()\nlet xs=[unwrap(r)]\n',
                 'let r Result<void>=ok()\nlet a,b=unwrap(r),1\n',
                 'let r Result<void>=ok()\nprint(unwrap(r)==nil)\n'):
        reject(text,"E3004")
    dynamic=fixture('fn unpack(r) {return unwrap(r)}\nlet r Result<void>=ok()\nprint(unpack(r))\n')
    run("check",dynamic,"OK\n");run("build",dynamic)
    for command in ("interpret","run"):run(command,dynamic,error="E4003")
    dynamic.unlink();run("run",dynamic.with_suffix(".huab"),error="E4003")
    prefix='import std.error as e\n'
    compare(prefix+'let x=unwrap(e.make("fs","not_found","missing"))\nlet y=e.with_context(e.with_origin(e.with_native(x,"win32",2),"load_config"),"read settings")\nprint(e.domain(y),e.code(y),e.message(y),e.origin(y),e.native_domain(y),e.native_code(y),e.contexts(y),e.contexts(x),e.cause(y),e.truncated(y))\n',"fs not_found missing load_config win32 2 [read settings] [] nil false\n")
    compare(prefix+'let x=e.from_string("FS_NOT_FOUND: missing","old.fs")\nprint(e.domain(x),e.code(x),e.message(x),e.origin(x))\nlet y=unwrap(e.wrap("app.config","load","cannot load",x))\nprint(e.domain(y),e.domain(unwrap(e.cause(y))),e.format(y))\n',"legacy failure FS_NOT_FOUND: missing old.fs\napp.config legacy app.config.load: cannot load\ncaused by: legacy.failure: FS_NOT_FOUND: missing [old.fs]\n")
    compare(prefix+'fn broken() Result<void,e.Value> {return err(e.from_string("failure","old"))}\nfn forwarded() Result<void,e.Value> {broken()?\nreturn ok()}\nprint(e.code(unwrap_err(forwarded())))\nlet r Result<void,e.Value>=ok()\nprint(r)\n',"failure\nok()\n")
    compare(prefix+'struct Error {message string}\nlet custom=Error{message:"mine"}\nprint(custom.message,e.code(e.from_string("old","")))\n',"mine failure\n")
    compare(prefix+'var x=e.from_string("root","")\nfor i in 0..40 {x=unwrap(e.wrap("app","wrapped","layer",x))}\nvar at e.Value?=x\nvar count=0\nwhile at!=nil {count++\nat=e.cause(unwrap(at))}\nprint(count,e.truncated(x))\n',"16 true\n")
    compare(prefix+'var x=e.from_string("message","")\nfor i in 0..32 {x=e.with_context(x,"context")}\nprint(len(e.contexts(x)),e.truncated(x))\n',"16 true\n")
    compare('import std.error as e\nimport std.strings as s\nvar text=""\nfor i in 0..2000 {text+="中文"}\nlet x=e.from_string(text,"")\nprint(s.len_bytes(e.message(x)),is_ok(s.rune_count(e.message(x))),e.truncated(x))\n',"4095 true true\n")
    compare(prefix+'print(is_err(e.make("FS","not_found","x")),is_err(e.make("fs","bad code","x")))\n',"true true\n")
    compare(prefix+'async fn work(x e.Value) Result<void,e.Value> {return err(e.with_context(x,"worker"))}\nfn main() {let source=e.from_string("bad","")\nlet t=work(source)\nlet x=unwrap_err(await t)\nprint(e.contexts(source),e.contexts(x),e.contexts(unwrap_err(await t)))}\n',"[] [worker] [worker]\n")
    compare(prefix+'import std.gc as gc\nvar x=e.from_string("root","")\nfor i in 0..600 {x=e.with_context(x,"ctx")\ngc.collect()}\nprint(e.message(x),len(e.contexts(x)),e.truncated(x))\n',"root 16 true\n")
    reject(prefix+'let x e.Value="bad"\n',"E3004")
    reject(prefix+'let x=e.Value{}\n',"E3004")
    reject(prefix+'let x=e.from_string("bad","")\nx.message="changed"\n',"E3004")
    reject(prefix+'fn broken() Result<void,e.Value> {return err("bad")}\n',"E3004")
    artifact=compare('fn ready() Result<void> {return ok()}\nprint(ready())\n',"ok()\n")
    raw=bytearray(artifact.read_bytes());struct.pack_into("<I",raw,8,6);artifact.write_bytes(raw)
    run("check",artifact,error="E6002");run("run",artifact,error="E6002")
report={"cases":cases,"checks":checks,"result":"passed","platform":"Windows x64","engines":["AST","VM","source-free HUAB"],"huab":[6,7]}
(build/"er-foundation").mkdir(exist_ok=True)
(build/"er-foundation/execution-verification.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
print(json.dumps(report))
