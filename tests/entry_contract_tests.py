"""A06 public entry/diagnostics contracts through AST, VM and source-free HUAB."""
from pathlib import Path
import hashlib, json, os, struct, sys, time
from process_guard import run, TestProcessTimeout

exe, root, build=(Path(x).resolve() for x in sys.argv[1:])
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='180'
work=build/'a06-entry';work.mkdir(exist_ok=True)
checks=0;rows=[];commands=[];started=time.monotonic()
def require(ok, detail):
    global checks
    checks+=1
    if not ok:raise AssertionError(detail)
def call(arguments, code=0, output=None, contains=(), absent=()):
    p=run([str(exe),*map(str,arguments)],capture_output=True,encoding='utf-8',errors='strict',timeout=10)
    commands.append({'args':list(map(str,arguments)),'code':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
    require(p.returncode==code,commands[-1])
    if output is not None:require(p.stdout==output,commands[-1])
    for text in contains:require(text in p.stderr,commands[-1])
    for text in absent:require(text not in p.stderr,commands[-1])
    if not contains and code==0:require(not p.stderr,commands[-1])
    return p
def proof(name, source, code=0, output='', contains=(), absent=(), version=None):
    folder=work/name;folder.mkdir(exist_ok=True)
    path=folder/'中文 入口.hua';artifact=path.with_suffix('.huab')
    path.write_text(source,encoding='utf-8',newline='\n')
    call(['check',path],output='OK\n')
    call(['build',path,'-o',artifact])
    require(artifact.exists(),'missing archive')
    if version:require(struct.unpack_from('<I',artifact.read_bytes(),8)[0]==version,'HUAB version changed')
    results=[call([cmd,path],code,output,contains,absent) for cmd in ('interpret','run')]
    hidden=path.with_suffix('.hidden');path.rename(hidden)
    try:
        require(not path.exists(),'source still present')
        results.append(call(['run',artifact],code,output,contains,absent))
    finally:hidden.rename(path)
    require(all(p.stderr==results[0].stderr for p in results),'engine failure reports differ')
    rows.append({'case':name,'code':code,'status':'passed'})
def reject(name, source, diagnostic):
    p=work/(name+'.hua');p.write_text(source,encoding='utf-8',newline='\n');out=p.with_suffix('.huab')
    out.unlink(missing_ok=True)
    for command in ('check','interpret','run','build'):
        call([command,p],1,'',[diagnostic])
    require(not out.exists(),'rejected source published an archive')
    rows.append({'case':name,'code':1,'status':'passed'})
def save(status):
    report={'status':status,'cases':len(rows),'checks':checks,'commands':len(commands),
            'seconds':round(time.monotonic()-started,3),'engines':['AST','VM','source-free HUAB'],
            'hua_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'rows':rows}
    (work/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (work/'commands.json').write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='rows'},ensure_ascii=False),flush=True)
try:
    proof('void_main','fn main(){print("Hello, Hua")}\n',output='Hello, Hua\n',version=6)
    proof('explicit_void','fn main() void{print("done")\nreturn}\n',output='done\n')
    proof('no_main','print("top level")\n',output='top level\n')
    proof('int_ignored','fn main() int{print("done")\nreturn 7}\n',output='done\n')
    proof('optional_ignored','fn main() int?{return nil}\n')
    proof('ordinary_error_handled','fn fail() Result<void>{return err("handled")}\nfn main(){print(unwrap_err(fail()))}\n',output='handled\n')
    proof('result_void_ok','fn main() Result<void>{print("done")\nreturn ok()}\n',output='done\n',version=7)
    proof('result_value_ok','fn main() Result<int>{return ok(7)}\n',version=6)
    proof('result_value_err','fn main() Result<int>{return err("failure")}\n',1,contains=['error[E0003]','failure'])
    proof('string_err','fn main() Result<void>{return err("输入失败，路径不存在")}\n',1,contains=['error[E0003]','输入失败，路径不存在'],version=7)
    proof('message_does_not_classify','fn main() Result<void>{return err("hua.cli.internal E0002 E8203")}\n',1,contains=['error[E0003]'])
    proof('propagate_cleanup','fn fail() Result<int>{return err("propagated")}\nfn main() Result<void>{defer print("outer")\n{defer print("inner")\nlet value=fail()?\nprint(value)}\nreturn ok()}\n',1,'inner\nouter\n',['propagated','error[E0003]'])
    proof('cleanup_replaces_err','fn broken(){print("cleanup")\nprint(1//0)}\nfn main() Result<void>{defer print("remaining")\ndefer broken()\nreturn err("business")}\n',1,'cleanup\nremaining\n',['error[E4004]'],['E0003','business'])
    proof('cleanup_replaces_ok','fn broken(){print(unwrap(err("cleanup")))}\nfn main() Result<void>{defer broken()\nreturn ok()}\n',1,contains=['error[E4009]'],absent=['E0003'])
    proof('original_diagnostic_priority','fn broken(){print(unwrap(err("cleanup")))}\nfn main() Result<void>{defer broken()\nprint(1//0)\nreturn err("unreachable")}\n',1,contains=['error[E4004]'],absent=['E0003','E4009'])
    prefix='import std.error as e\n'
    for kind,code in [('input',1),('usage',2),('internal',3)]:
        proof('structured_'+kind,prefix+'fn main() Result<void,e.Value>{return err(unwrap(e.make("hua.cli","'+kind+'","detail")))}\n',code,contains=['error[E0003]','hua.cli.'+kind+': detail'])
    proof('foreign_identity_not_classified',prefix+'fn main() Result<void,e.Value>{return err(unwrap(e.make("app","internal","detail")))}\n',1,contains=['app.internal: detail'])
    proof('root_classification',prefix+'fn main() Result<void,e.Value>{let cause=unwrap(e.make("hua.cli","internal","cause"))\nreturn err(unwrap(e.wrap("app","failure","root",cause)))}\n',1,contains=['app.failure: root','caused by: hua.cli.internal: cause'])
    proof('structured_context',prefix+'fn main() Result<void,e.Value>{let cause=e.from_string("old failure","old.api")\nlet root=unwrap(e.wrap("compiler","input","源文件失败",cause))\nreturn err(e.with_native(e.with_origin(e.with_context(root,"读取入口"),"input.hua:2"),"win32",5))}\n',1,contains=['compiler.input: 源文件失败','context: 读取入口','[input.hua:2]','[win32:5]','caused by: legacy.failure: old failure [old.api]'])
    proof('custom_error_no_payload_log','struct Failure{secret string}\nfn main() Result<void,Failure>{return err(Failure{secret:"do not log"})}\n',1,contains=['error[E0003]','<Failure>'],absent=['do not log'])
    proof('async_void','async fn main(){print("async")}\n',output='async\n')
    proof('async_result_ok','async fn main() Result<void>{return ok()}\n')
    proof('async_result_err','async fn main() Result<void>{defer print("cleanup")\nreturn err("async failure")}\n',1,'cleanup\n',['async failure','error[E0003]'])
    proof('unobserved_task_diagnostic','async fn broken(){print(1//0)}\nfn main() Result<void>{let task=broken()\nreturn err("business")}\n',1,contains=['error[E4004]'],absent=['E0003'])
    proof('task_business_result_stays_value','async fn fail() Result<void>{return err("worker business")}\nfn main(){let task=fail()\nprint(await task)}\n',output='err(worker business)\n')
    reject('main_parameters','print("BAD")\nfn main(x int){}\n','error[E3005]')
    reject('result_fallthrough','print("BAD")\nfn main() Result<void>{print("missing return")}\n','error[E3006]')
    reject('result_void_payload','print("BAD")\nfn main() Result<void>{return ok(nil)}\n','error[E3004]')
    reject('error_type_mismatch',prefix+'print("BAD")\nfn fail() Result<void>{return err("x")}\nfn main() Result<void,e.Value>{fail()?\nreturn ok()}\n','error[E3004]')
    for args in ([],['run'],['unknown','file'],['build','missing.hua','-o','wrong.json']):
        call(args,2,'',['error[E0004]','Usage:'])
    call(['check',work/'missing.hua'],1,'',['error[E0001]'])
    good=work/'publication.hua';good.write_text('print("MUST NOT EXECUTE")\n',encoding='utf-8')
    bad=work/'malformed.hua';bad.write_text('let =\n',encoding='utf-8')
    old=work/'preserved.huab';old.write_bytes(b'old artifact')
    call(['build',bad,'-o',old],1,'',['error[E2001]'])
    require(old.read_bytes()==b'old artifact','failed compile replaced previous output')
    target=work/'missing-directory'/'output.huab'
    call(['build',good,'-o',target],1,'',['error[E0001]'])
    require(not target.exists(),'failed publication produced output')
    directory=work/'directory.huab';directory.mkdir(exist_ok=True)
    marker=directory/'keep.txt';marker.write_text('keep',encoding='utf-8')
    call(['build',good,'-o',directory],1,'',['error[E0001]'])
    require(marker.read_text()=='keep','failed replacement changed target')
    require(not list(work.glob('directory.huab.tmp-*')),'failed publication left temporary files')
    call(['check',old],1,'',['error[E6002]'])
    call(['run',old],1,'',['error[E6002]'])
    for command in ('check','bytecode','ast'):
        call([command,work/'string_err'/'中文 入口.hua'])
    rows.append({'case':'cli_errors_and_atomic_output','status':'passed'})
except TestProcessTimeout:
    save('timeout');raise
except BaseException:
    save('failed');raise
save('passed')
