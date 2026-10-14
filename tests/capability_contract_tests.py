"""A07 native capability contracts. No compiler bootstrap/generation runs."""
from pathlib import Path
import hashlib,json,os,sys,time
from process_guard import run,TestProcessTimeout
exe,root,build=(Path(x).resolve() for x in sys.argv[1:4]);work=build/'a07-completion';work.mkdir(exist_ok=True)
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='180'
cases=json.loads((root/'tests/data/a07_completion_cases.json').read_text(encoding='utf-8'))
checks=0;rows=[];commands=[];started=time.monotonic()
def require(ok,detail):
    global checks
    checks+=1
    if not ok:raise AssertionError(detail)
def call(args,code=0,out=None):
    p=run([str(exe),*map(str,args)],capture_output=True,encoding='utf-8',timeout=10)
    commands.append(dict(args=list(map(str,args)),code=p.returncode,stdout=p.stdout,stderr=p.stderr))
    require(p.returncode==code,commands[-1])
    if out is not None:require(p.stdout==out,commands[-1])
    return p
def matrix(c):
    d=work/c['name'];d.mkdir(exist_ok=True);s=d/'main.hua';s.write_text(c['source'],encoding='utf-8',newline='\n');a=d/'program.huab'
    if c['expected']!='ok':
        a.write_bytes(b'keep existing output')
        for mode in ('check','interpret','run','build'):
            args=[mode,s]
            if mode=='build':args+=['-o',a]
            p=call(args,1,'');require('error['+c['expected']+']' in p.stderr,commands[-1]);require(' --> ' in p.stderr and '^' in p.stderr,'unlocated capability diagnostic')
        require(a.read_bytes()==b'keep existing output','static rejection changed output')
    else:
        call(['check',s],out='OK\n');call(['build',s,'-o',a])
        for mode in ('interpret','run'):call([mode,s],out=c['stdout'])
        hidden=s.with_suffix('.hidden');s.rename(hidden)
        try:call(['check',a],out='OK\n');call(['run',a],out=c['stdout'])
        finally:hidden.rename(s)
    rows.append(dict(case=c['name'],status='passed'))
failures=[]
try:
    for c in cases:
        try:matrix(c)
        except AssertionError as error:rows.append(dict(case=c['name'],status='failed',error=str(error)));failures.append(c['name'])
except TestProcessTimeout:raise
finally:
    result=dict(status='failed' if failures else 'passed',cases=len(rows),checks=checks,commands=len(commands),seconds=round(time.monotonic()-started,3),hua_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),rows=rows)
    (work/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    (work/'commands.json').write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'}),flush=True)
if failures:print('Failed:',', '.join(failures));raise SystemExit(1)
