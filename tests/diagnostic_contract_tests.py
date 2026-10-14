"""A10 native source/diagnostic contract; no compiler bootstrap pipeline."""
from pathlib import Path
import hashlib,json,os,re,sys,time
from process_guard import run,TestProcessTimeout
exe,root,build=(Path(x).resolve() for x in sys.argv[1:4]);red='--red' in sys.argv
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='180'
work=build/'a10-diagnostics';work.mkdir(exist_ok=True)
cases=json.loads((root/'tests/data/a10_diagnostic_cases.json').read_text(encoding='utf-8'))
checks=0;rows=[];commands=[];started=time.monotonic()
def require(ok,detail):
    global checks
    checks+=1
    if not ok:raise AssertionError(detail)
def call(args,code=0,out=None,cwd=None):
    p=run([str(exe),*map(str,args)],capture_output=True,encoding='utf-8',errors='strict',timeout=10,cwd=cwd,env=dict(os.environ,HUA_TASK_WORKERS='1'))
    commands.append({'args':list(map(str,args)),'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
    require(p.returncode==code,commands[-1])
    if out is not None:require(p.stdout==out,commands[-1])
    return p
def normalize(raw):return raw.removeprefix(b'\xef\xbb\xbf').replace(b'\r\n',b'\n').replace(b'\r',b'\n')
def matrix(c):
    directory=work/c['name'];directory.mkdir(exist_ok=True);entry=directory/c.get('filename','main.hua')
    raw=bytes.fromhex(c['raw_hex']) if c.get('raw_hex') else c['source'].encode('utf-8');entry.write_bytes(raw)
    for name,text in c['files'].items():(directory/name).write_text(text,encoding='utf-8',newline='\n')
    origin=directory/c.get('origin',entry.name);text=normalize(origin.read_bytes())
    if c.get('position'):position=tuple(c['position'])
    else:
        offset=text.index(c['mark'].encode('utf-8'))+c.get('adjust',0)
        position=(text[:offset].count(b'\n')+1,offset-(text.rfind(b'\n',0,offset)+1)+1)
    def observe(p):
        require('error['+c['error']+']' in p.stderr,commands[-1]);match=re.search(r'^ --> (.+):(\d+):(\d+)$',p.stderr,re.M)
        require(match is not None,commands[-1]);require((int(match[2]),int(match[3]))==position,{'case':c['name'],'expected':position,'actual':match.groups()})
        require(Path(match[1]).name==origin.name,'wrong source file')
        if not c.get('entry'):require('^' in p.stderr and ' | ' in p.stderr,'missing source excerpt')
        for note in c['notes']:require('note: '+note in p.stderr,commands[-1])
        for fragment in c.get('fragments',[]):require(fragment in p.stderr,commands[-1])
        require('\x00' not in p.stderr,'raw NUL in rendered diagnostic')
    archive=directory/'program.huab'
    if c['phase']=='check':
        archive.write_bytes(b'previous artifact')
        modes=('ast','check','interpret','run','build') if c['error'].startswith(('E1','E2')) and origin==entry else ('check','interpret','run','build')
        for mode in modes:
            args=[mode,entry]
            if mode=='build':args+=['-o',archive]
            observe(call(args,1,''))
        require(archive.read_bytes()==b'previous artifact','failed input replaced previous output')
    else:
        call(['check',entry],out='OK\n');call(['build',entry,'-o',archive])
        reports=[]
        for mode in ('interpret','run'):
            p=call([mode,entry],1,c['stdout'],directory);observe(p);reports.append(p.stderr)
        hidden=[]
        try:
            for p in directory.glob('*.hua'):
                q=p.with_name(p.name+'.hidden');p.rename(q);hidden.append((p,q))
            require(not list(directory.glob('*.hua')),'source-free gate')
            call(['check',archive],out='OK\n');p=call(['run',archive],1,c['stdout'],directory);observe(p);reports.append(p.stderr)
            require(all(x==reports[0] for x in reports),'engine diagnostic reports differ')
        finally:
            for p,q in hidden:q.rename(p)
    rows.append({'case':c['name'],'status':'passed'})
def save(status):
    report={'status':status,'cases':len(rows),'checks':checks,'commands':len(commands),'seconds':round(time.monotonic()-started,3),
            'hua_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'rows':rows}
    prefix='red-' if red else ''
    (work/(prefix+'results.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    (work/(prefix+'commands.json')).write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({k:v for k,v in report.items() if k!='rows'}),flush=True)
try:
    failed=False
    for c in cases:
        if red and c['name'] not in ('void_copy_location','nested_calls','result_entry','module_parse'):continue
        try:matrix(c)
        except AssertionError as error:
            rows.append({'case':c['name'],'status':'failed','error':str(error)});failed=True
            if not red:raise
    save('failed' if failed else 'passed')
    if failed:raise SystemExit(1)
except BaseException as error:
    if not isinstance(error,SystemExit):save('timeout' if isinstance(error,TestProcessTimeout) else 'failed')
    raise
