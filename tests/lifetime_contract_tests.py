"""A09 bounded lifetime contracts across native AST, VM and source-free HUAB."""
from pathlib import Path
import hashlib,json,os,sys,time
from process_guard import run,TestProcessTimeout
if len(sys.argv)>1 and sys.argv[1]=='--file-probe':
    p=Path(sys.argv[2])
    if os.name=='nt':
        import ctypes
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
        kernel.CreateFileW.restype=ctypes.c_void_p
        handle=kernel.CreateFileW(str(p),0x80000000,0,None,3,0,None)
        if handle==ctypes.c_void_p(-1).value:raise ctypes.WinError(ctypes.get_last_error())
        kernel.CloseHandle.argtypes=[ctypes.c_void_p];kernel.CloseHandle.restype=ctypes.c_int
        if not kernel.CloseHandle(handle):raise ctypes.WinError(ctypes.get_last_error())
    else:
        with p.open('rb'):pass
    raise SystemExit(0)
exe,root,build=(Path(x).resolve() for x in sys.argv[1:4])
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='180'
work=build/'a09-lifetime';work.mkdir(exist_ok=True)
cases=json.loads((root/'tests/data/a09_lifetime_cases.json').read_text(encoding='utf-8'))
checks=0;rows=[];commands=[];started=time.monotonic()
def require(ok,detail):
    global checks
    checks+=1
    if not ok:raise AssertionError(detail)
def call(args,code=0,out=None,error=None,cwd=None,located=False):
    p=run([str(exe),*map(str,args)],capture_output=True,encoding='utf-8',timeout=10,cwd=cwd,env=dict(os.environ,HUA_TASK_WORKERS='1'))
    commands.append({'args':list(map(str,args)),'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
    require(p.returncode==code,commands[-1])
    if out is not None:require(p.stdout==out,commands[-1])
    if error:
        require('error['+error+']' in p.stderr,commands[-1])
        if error=='E4099':require('File' not in p.stderr,commands[-1])
        if located:require(' --> ' in p.stderr and '^' in p.stderr,commands[-1])
    else:require(not p.stderr,commands[-1])
    return p
def matrix(case):
    directory=work/case['name'];directory.mkdir(exist_ok=True)
    source=directory/'main.hua';source.write_text(case['source'],encoding='utf-8',newline='\n')
    archive=directory/'program.huab';call(['check',source],out='OK\n');call(['build',source,'-o',archive])
    code=1 if case.get('runtime_error') else 0
    try:
        for label,mode,path in (('ast','interpret',source),('vm','run',source),('huab','run',archive)):
            cwd=directory/label;cwd.mkdir(exist_ok=True)
            if label=='huab':
                source.rename(source.with_suffix('.hidden'));require(not source.exists(),'source-free execution');call(['check',archive],out='OK\n')
            for name in case.get('files',{}):
                target=cwd/name
                if not target.resolve().is_relative_to(cwd.resolve()):raise ValueError('file fixture escapes test directory')
                target.unlink(missing_ok=True)
            call([mode,path],code,case['stdout'],case.get('runtime_error'),cwd,case.get('located',False))
            for name,data in case.get('files',{}).items():
                p=cwd/name;require(p.read_bytes()==data.encode('utf-8'),'wrong file contents')
                # Verify post-exit access; native resource tests prove close/retirement before exit.
                probe=run([sys.executable,str(root/'tests/lifetime_contract_tests.py'),'--file-probe',str(p)],capture_output=True,encoding='utf-8',timeout=10)
                require(probe.returncode==0,probe.stderr)
    finally:
        hidden=source.with_suffix('.hidden')
        if hidden.exists():hidden.rename(source)
    rows.append({'case':case['name'],'status':'passed'})
def save(status):
    result={'status':status,'cases':len(rows),'checks':checks,'commands':len(commands),'seconds':round(time.monotonic()-started,3),
            'hua_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'rows':rows}
    (work/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    (work/'commands.json').write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'}),flush=True)
try:
    for case in cases:matrix(case)
except BaseException as error:
    rows.append({'case':case['name'],'status':'failed','error':str(error)});save('timeout' if isinstance(error,TestProcessTimeout) else 'failed');raise
save('passed')
