"""Public Hua compiler entry: no Python report parsing is needed for failure status."""
from pathlib import Path
import hashlib, json, os, shutil, sys, time
from process_guard import run, TestProcessTimeout
exe,root,build=(Path(x).resolve() for x in sys.argv[1:])
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='240'
work=build/'a06-compiler';work.mkdir(exist_ok=True)
commands=[];checks=0;started=time.monotonic()
def require(ok,detail):
    global checks
    checks+=1
    if not ok:raise AssertionError(detail)
def call(args,code=0,output=None,error=None):
    p=run([str(exe),*map(str,args)],capture_output=True,encoding='utf-8',errors='strict',timeout=30)
    commands.append({'args':list(map(str,args)),'code':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
    require(p.returncode==code,commands[-1])
    if output is not None:require(p.stdout==output,commands[-1])
    if error:require(error in p.stderr,commands[-1])
    elif code==0:require(not p.stderr,commands[-1])
    return p
def save(status):
    result={'status':status,'checks':checks,'commands':len(commands),'seconds':round(time.monotonic()-started,3),
            'hua_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'engines':['AST','VM','source-free compiler HUAB']}
    (work/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    (work/'commands.json').write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result),flush=True)
try:
    entry=root/'selfhost/compiler_main.hua'
    call(['check',entry],output='OK\n')
    component_dir=work/'component';component_dir.mkdir(exist_ok=True)
    copies=[]
    for original in (root/'selfhost').glob('*.hua'):
        copied=component_dir/original.name;shutil.copy2(original,copied);copies.append(copied)
    component=component_dir/'compiler.huab'
    call(['build',component_dir/entry.name,'-o',component])
    for copied in copies:copied.rename(copied.with_suffix('.hidden'))
    try:
        require(not list(component_dir.glob('*.hua')),'compiler source was not removed')
        for index,(action,compiler) in enumerate([('interpret',entry),('run',entry),('run',component)]):
            folder=work/f'path{index}';folder.mkdir(exist_ok=True)
            source=folder/'输入 测试.hua';source.write_text('fn main(){print("compiled")\nprint(7//2)}\n',encoding='utf-8')
            target=folder/'输出 测试.huab'
            prefix=[action,compiler,'--execution-profile','compiler','--']
            call(prefix+['--help'],output='Usage: compiler_main.hua -- build entry.hua -o output.huab\n')
            call(prefix,2,'','hua.cli.usage')
            call(prefix+['build',source,'-o',target.with_suffix('.json')],2,'','hua.cli.usage')
            call(prefix+['build',source,'-o',target],output='Built '+str(target)+'\n')
            require(target.exists(),'successful compile published no artifact')
            call(['check',target],output='OK\n')
            source.rename(source.with_suffix('.hidden'))
            try:call(['run',target],output='compiled\n3\n')
            finally:source.with_suffix('.hidden').rename(source)
            before=target.read_bytes()
            source.write_text('let =\n',encoding='utf-8')
            p=call(prefix+['build',source,'-o',target],1,'','hua.cli.input')
            require('E2001@' in p.stderr or 'E2002@' in p.stderr,commands[-1])
            require(target.read_bytes()==before,'invalid input replaced previous artifact')
            call(prefix+['build',folder/'missing.hua','-o',target],1,'','hua.cli.input')
            require(target.read_bytes()==before,'missing input replaced previous artifact')
            source.write_text('fn main(){print("must not execute during build")}\n',encoding='utf-8')
            call(prefix+['build',source,'-o',folder/'absent'/'out.huab'],1,'','compiler stage: publication')
            require(not (folder/'absent'/'out.huab').exists(),'publication failure published output')
            require(not list(folder.glob('hua-huab-*')),'publication left temporary files')
    finally:
        for copied in copies:copied.with_suffix('.hidden').rename(copied)
except TestProcessTimeout:
    save('timeout');raise
except BaseException:
    save('failed');raise
save('passed')
