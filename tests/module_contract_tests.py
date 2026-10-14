"""A08 native module contracts. No self-host or generation pipeline is run."""
from pathlib import Path
import hashlib,json,os,sys,time
from process_guard import run,TestProcessTimeout
exe,root,build=(Path(x).resolve() for x in sys.argv[1:4]);red='--red' in sys.argv
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='180'
work=build/'a08-modules';work.mkdir(exist_ok=True)
cases=json.loads((root/'tests/data/a08_module_cases.json').read_text(encoding='utf-8'))
checks=0;rows=[];commands=[];started=time.monotonic()
def require(ok,detail):
    global checks
    checks+=1
    if not ok:raise AssertionError(detail)
def call(args,code=0,output=None,error=None,cwd=None):
    p=run([str(exe),*map(str,args)],capture_output=True,encoding='utf-8',timeout=10,cwd=cwd)
    commands.append({'args':list(map(str,args)),'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
    require(p.returncode==code,commands[-1])
    if output is not None:require(p.stdout==output,commands[-1])
    if error:require('error['+error+']' in p.stderr,commands[-1])
    elif code==0:require(not p.stderr,commands[-1])
    return p
def fixture(name,files):
    directory=work/name;directory.mkdir(exist_ok=True)
    for name,text in files.items():
        p=directory/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text,encoding='utf-8',newline='\n')
    return directory/'main.hua'
def matrix(case):
    entry=fixture(case['name'],case['files']);archive=entry.parent/'program.huab'
    if 'error' in case:
        archive.write_bytes(b'old output')
        for mode in ('check','interpret','run','build'):
            args=[mode,entry]
            if mode=='build':args+=['-o',archive]
            call(args,1,'',case['error'])
        require(archive.read_bytes()==b'old output','rejected graph changed output')
    else:
        call(['check',entry],output='OK\n')
        call(['build',entry,'-o',archive]);binary=archive.read_bytes()
        second=entry.parent/'second.huab';call(['build',entry,'-o',second],cwd=work)
        require(second.read_bytes()==binary,'same graph did not build deterministically')
        code=1 if case.get('runtime_error') else 0
        for mode in ('interpret','run'):call([mode,entry],code,case['stdout'],case.get('runtime_error'),cwd=work)
        hidden=[]
        try:
            for p in list(entry.parent.rglob('*')):
                if p.suffix not in ('.hua','.wasm','.huam'):continue
                new=p.with_name(p.name+'.hidden');p.rename(new);hidden.append((p,new))
            require(not list(entry.parent.rglob('*.hua')),'source still present')
            call(['check',archive],output='OK\n')
            call(['run',archive],code,case['stdout'],case.get('runtime_error'),cwd=work)
        finally:
            for old,new in hidden:new.rename(old)
    rows.append({'case':case['name'],'status':'passed'})
def save(status):
    result={'status':status,'cases':len(rows),'checks':checks,'commands':len(commands),
            'seconds':round(time.monotonic()-started,3),'hua_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'rows':rows}
    (work/('red-results.json' if red else 'results.json')).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    (work/('red-commands.json' if red else 'commands.json')).write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'}),flush=True)
try:
    failures=[]
    for case in cases:
        if red and not case['name'].startswith('overlap_'):continue
        try:matrix(case)
        except AssertionError as failure:
            rows.append({'case':case['name'],'status':'failed','error':str(failure)});failures.append(case['name'])
    if not red and not failures:
        # Directory aliases are real filesystem identities, not mocked path strings.
        def directory_link(link,target):
            if link.exists():
                require(link.resolve()==target.resolve(),'unexpected existing directory alias')
                return
            if os.name=='nt':
                quoted=lambda p:"'"+str(p).replace("'","''")+"'"
                command='New-Item -ItemType Junction -Path '+quoted(link)+' -Target '+quoted(target)+' | Out-Null'
                created=run(['powershell.exe','-NoProfile','-NonInteractive','-Command',command],capture_output=True,encoding='utf-8',timeout=10)
                require(created.returncode==0,created.stderr)
            else:link.symlink_to(target,target_is_directory=True)
            require(link.resolve()==target.resolve(),'directory alias was not created')
        alias=fixture('canonical_alias',{'main.hua':'import pkg as a\nimport alias as b\nprint(a.get(),b.get())\n','pkg/package.hua':'print("once")\npub fn get() int{return 1}\n'})
        directory_link(alias.parent/'alias',alias.parent/'pkg')
        archive=alias.parent/'program.huab'
        call(['check',alias],output='OK\n');call(['build',alias,'-o',archive])
        for mode in ('interpret','run'):call([mode,alias],output='once\n1 1\n')
        source=alias.parent/'pkg/package.hua';source.rename(source.with_suffix('.hidden'));alias.rename(alias.with_suffix('.hidden'))
        try:call(['run',archive],output='once\n1 1\n')
        finally:
            source.with_suffix('.hidden').rename(source);alias.with_suffix('.hidden').rename(alias)
        rows.append({'case':'canonical_directory_alias','status':'passed'})
        escaped=fixture('root_escape',{'main.hua':'import escaped.mod\nprint("BAD")\n'})
        outside=work/'outside-root';outside.mkdir(exist_ok=True);(outside/'mod.hua').write_text('print("BAD")\n',encoding='utf-8',newline='\n')
        directory_link(escaped.parent/'escaped',outside)
        archive=escaped.parent/'old.huab';archive.write_bytes(b'keep')
        for mode in ('check','interpret','run','build'):
            args=[mode,escaped]
            if mode=='build':args+=['-o',archive]
            call(args,1,'','E5001')
        require(archive.read_bytes()==b'keep','root escape replaced output')
        rows.append({'case':'canonical_root_escape','status':'passed'})
        for count,wanted in ((127,0),(128,1)):
            files={'main.hua':''.join('import count'+str(i)+'\n' for i in range(count))}
            files.update({f'count{i}.hua':'' for i in range(count)})
            p=fixture('count_'+str(count),files);call(['check',p],wanted,'OK\n' if not wanted else '', 'E5005' if wanted else None)
            rows.append({'case':'module_count_'+str(count+1),'status':'passed'})
        for depth,wanted in ((63,0),(64,1)):
            files={'main.hua':'import chain0\n'}
            for i in range(depth):files[f'chain{i}.hua']='import chain'+str(i+1)+'\n' if i+1<depth else ''
            p=fixture('depth_'+str(depth),files);call(['check',p],wanted,'OK\n' if not wanted else '', 'E5005' if wanted else None)
            rows.append({'case':'module_depth_'+str(depth+1),'status':'passed'})
        p=fixture('single_file_limit',{'main.hua':'import huge\n','huge.hua':''});(p.parent/'huge.hua').write_bytes(b' '*(16*1024*1024+1));call(['check',p],1,'','E5005');rows.append({'case':'single_file_limit','status':'passed'})
        p=fixture('total_size_limit',{'main.hua':'import big0\nimport big1\nimport big2\nimport big3\n'})
        for i in range(4):(p.parent/f'big{i}.hua').write_bytes(b' '*(16*1024*1024))
        call(['check',p],1,'','E5005');rows.append({'case':'total_size_limit','status':'passed'})
    save('failed' if failures else 'passed')
    if failures:raise SystemExit(1)
except TestProcessTimeout:
    save('timeout');raise
except BaseException:
    if not isinstance(sys.exc_info()[1],SystemExit):save('failed')
    raise
