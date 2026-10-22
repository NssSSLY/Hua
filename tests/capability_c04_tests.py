"""A07 C04 parity on small inputs only; no own-compiler/generation pipeline."""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess,sys,time
from process_guard import run,TestProcessTimeout
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('exe',type=Path);parser.add_argument('root',type=Path);parser.add_argument('build',type=Path)
parser.add_argument('--unbounded',action='store_true',help='Explicitly disable test subprocess/suite wall-clock limits; compiler limits stay unchanged')
parser.add_argument('--batch-size',type=int,default=8,help='Inputs per guarded checker invocation (default: 8)')
parser.add_argument('--cases',nargs='+',help='Run named cases for focused diagnosis')
parser.add_argument('--extra-cases',type=Path,help='Additional small regression cases using the same native/report comparison')
parser.add_argument('--compatibility',action='store_true',help='Also compare existing small C04 type contracts')
parser.add_argument('--execution-probe',type=Path,help='Prepare the native frontend once, then run the unchanged AST/VM with separate phase watchdogs')
parser.add_argument('--prepare-seconds',type=float,default=60,help='Native source preparation watchdog only; max 120, executor watchdog unchanged')
parser.add_argument('--prepared',type=Path,help='Reuse a component with an exact adjacent .sources.json manifest')
parser.add_argument('--reference-component',type=Path,help='Compare complete reports with a frozen previous component')
parser.add_argument('--child-seconds',type=float,default=30,help='Bound each child, including source frontend work; max 60')
parser.add_argument('--subset-probe',type=Path,help='Also compare the SB1 guarded host path')
parser.add_argument('--type-oracle',type=Path,help='Compare positive node type/capability facts with the independent native oracle')
options=parser.parse_args();exe,root,build=(x.resolve() for x in (options.exe,options.root,options.build));work=build/'a07-c04';work.mkdir(parents=True,exist_ok=True)
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='900'
cases=json.loads((root/'tests/data/a07_completion_cases.json').read_text(encoding='utf-8'))
if options.compatibility:cases+=json.loads((root/'tests/data/a07_c04_type_cases.json').read_text(encoding='utf-8'))
if options.extra_cases:cases+=json.loads(options.extra_cases.read_text(encoding='utf-8'))
assert len({c['name'] for c in cases})==len(cases),'Duplicate test identity'
excluded={'method_downgrade':'T1 excludes methods','method_refresh':'T1 excludes methods','prior_error_payload':'T1 excludes non-string Result error','unknown_callback_global':'T1 requires annotated parameters'}
for case in cases:
    if case.get('c04_excluded'):excluded[case['name']]=case['c04_excluded']
selected=[c for c in cases if c['name'] not in excluded and (not options.cases or c['name'] in options.cases)]
if options.cases:assert set(options.cases)=={c['name'] for c in selected},'Unknown or excluded selected case'
assert selected,'No small inputs selected'
assert options.batch_size>0,'Batch size must be positive'
assert 0<options.child_seconds<=60
assert 0<options.prepare_seconds<=120
os.environ['HUA_TEST_TIMEOUT_SECONDS']=str(options.child_seconds)
checks=0;commands=[];rows=[];failures=[];started=time.monotonic()
stamp=str(time.time_ns());log=work/('commands-'+stamp+'.json')
def require(ok,detail):
    global checks
    checks+=1
    if not ok:raise AssertionError(detail)
def call(args,code=0,tool=None):
    t=time.monotonic();command=[str(tool or exe),*map(str,args)]
    if args[0]!='check' or len(commands)%20==0:print('C04 command '+str(len(commands)+1)+': '+str(args[0])+' '+str(args[1]),flush=True)
    try:p=(subprocess.run if options.unbounded else run)(command,capture_output=True,encoding='utf-8',timeout=None if options.unbounded else options.child_seconds)
    except BaseException as error:
        commands.append(dict(args=list(map(str,args)),status='failed',seconds=round(time.monotonic()-t,3),error=str(error)))
        log.write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
        raise
    commands.append(dict(args=list(map(str,args)),code=p.returncode,seconds=round(time.monotonic()-t,3),stdout=p.stdout,stderr=p.stderr))
    log.write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    require(p.returncode==code,commands[-1]);return p
modes=['interpret','run','huab']+(['sb1'] if options.subset_probe else [])+(['reference'] if options.reference_component else [])
reports={mode:{} for mode in modes}
try:
    analyzer=work/'analyzer';analyzer.mkdir(exist_ok=True)
    for p in (root/'selfhost').glob('*.hua'):shutil.copyfile(p,analyzer/p.name)
    driver=analyzer/'a07_main.hua'
    driver.write_text('import types as types\nimport type_report as report\nimport std.os as os\nimport std.path as path\nfn main(){for entry in os.args(){let result=types.analyze(entry)\nlet name=unwrap(path.basename(entry))\nif is_err(result){print(report.failure(name,unwrap_err(result)))}else{print(report.success(unwrap(result),name))}}}\n',encoding='utf-8',newline='\n')
    artifact=work/'analyzer.huab'
    if options.prepared:
        manifest=json.loads(options.prepared.with_suffix('.sources.json').read_text(encoding='utf-8'))
        require(hashlib.sha256(options.prepared.read_bytes()).hexdigest()==manifest['artifact_sha256'],'prepared artifact identity')
        require({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in analyzer.glob('*.hua')}==manifest['sources'],'prepared source identities')
        shutil.copyfile(options.prepared,artifact)
    elif not options.execution_probe:call(['build',driver,'-o',artifact])
    inputs=[];native={};references={}
    for c in selected:
        d=work/c['name'];d.mkdir(exist_ok=True);source=d/'main.hua';source.write_text(c['source'],encoding='utf-8',newline='\n');inputs.append(source)
        p=call(['check',source],0 if c['expected']=='ok' else 1)
        require(('error['+c['expected']+']') in p.stderr if c['expected']!='ok' else p.stdout=='OK\n',c['name'])
        match=re.search(r' --> .+:(\d+):(\d+)\n',p.stderr)
        native[c['name']]=tuple(map(int,match.groups())) if match else None
        if options.type_oracle and c['expected']=='ok':
            reference=json.loads(call(['--types',source],tool=options.type_oracle).stdout)
            require(reference['status']=='ok',(c['name'],reference))
            references[c['name']]=reference
    if options.execution_probe:
        from capability_prepared_support import execute
        trace=work/('prepared-'+stamp+'.trace')
        args=[options.execution_probe.resolve(),trace,driver]
        if not artifact.exists():args+=['--archive',artifact]
        args+=inputs
        p,measurement=execute(args,trace,work/('prepared-'+stamp+'.stdout'),work/('prepared-'+stamp+'.stderr'),options.child_seconds,started+890,prepare_limit=options.prepare_seconds)
        commands.append(dict(args=list(map(str,args)),status='prepared',**measurement))
        log.write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
        require(p.returncode==0,p.stderr)
        lines=p.stdout.splitlines();count=len(selected)
        require(len(lines)==2*count+2 and lines[0]=='@@prepared:AST' and lines[count+1]=='@@prepared:VM','prepared source protocol')
        for mode,chunk in [('interpret',lines[1:count+1]),('run',lines[count+2:])]:
            for c,raw in zip(selected,chunk):reports[mode][c['name']]=json.loads(raw)
    batch_size=len(selected) if options.unbounded else options.batch_size
    for offset in range(0,len(selected),batch_size):
        chunk=selected[offset:offset+batch_size];paths=inputs[offset:offset+batch_size]
        for mode in modes:
            if options.execution_probe and mode in ('interpret','run'):continue
            entry=options.reference_component.resolve() if mode=='reference' else artifact if mode=='huab' else driver
            p=call(['run' if mode in ('huab','sb1','reference') else mode,entry,'--execution-profile','compiler','--',*paths],tool=options.subset_probe if mode=='sb1' else None)
            decoded=[json.loads(line) for line in p.stdout.splitlines()]
            require(len(decoded)==len(chunk),(mode,'report count'))
            for c,report in zip(chunk,decoded):reports[mode][c['name']]=report
        for c in chunk:
            name=c['name'];r=reports['run'][name]
            try:
                require(all(r==reports[mode][name] for mode in modes),(name,'analyzer reports differ'))
                require(r.get('status')==('ok' if c['expected']=='ok' else 'error'),(name,r))
                if c['expected']!='ok':
                    match=re.match(re.escape(c['expected'])+r'@(\d+):(\d+)',r.get('error',''));require(match is not None,(name,r))
                    require(tuple(map(int,match.groups()))==native[name],(name,'source position differs',native[name],r))
                compared=0
                if name in references:
                    reference=references[name];facts={}
                    for fact in reference['facts']:facts.setdefault((fact['module'],fact['start'],fact['end']),[]).append(fact)
                    for fact in r['facts']:
                        candidates=facts.get((fact['module'],fact['start'],fact['end']),[])
                        if not candidates:continue
                        require(any(fact['type_name']==x['type_name'] and (x['writable'] is None or fact['writable']==x['writable']) for x in candidates),(name,'native node facts differ',fact,candidates));compared+=1
                    require(compared>0,(name,'no comparable native facts'))
                    functions={f['target']:f['writable'] for f in reference['functions']}
                    for function in r['functions']:require(function['writable']==functions[function['target']],(name,'native function fact differs',function,functions))
                rows.append(dict(case=name,status='passed',native_facts=compared))
            except AssertionError as error:
                rows.append(dict(case=name,status='failed',error=str(error)));failures.append(name)
                raise
        print('C04 small inputs',offset+len(chunk),'/',len(selected),flush=True)
    hidden=work/('hidden-analyzer-'+stamp)
    analyzer.rename(hidden)
    try:
        p=call(['run',artifact,'--execution-profile','compiler','--',inputs[0]])
        require(json.loads(p.stdout)==reports['run'][selected[0]['name']],'source-free analyzer changed report')
        if options.subset_probe:
            p=call(['run',artifact,'--execution-profile','compiler','--',inputs[0]],1,tool=options.subset_probe)
            require('error[E8101]' in p.stderr and not p.stdout,'SB1 must reject external HUAB before execution')
    finally:hidden.rename(analyzer)
except BaseException as error:
    failures.append('runner: '+str(error))
    raise
finally:
    result=dict(status='failed' if failures else 'passed',unbounded=options.unbounded,cases=len(rows),checks=checks,commands=len(commands),seconds=round(time.monotonic()-started,3),hua_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),scope='small T1 inputs; AST/VM/HUAB'+('/SB1' if options.subset_probe else '')+'; no compiler own source or generations',excluded=excluded,rows=rows)
    path=work/('results-'+stamp+'.json');path.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    (work/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},ensure_ascii=False),flush=True)
if failures:
    for row in rows:
        if row['status']=='failed':print(row['error'],flush=True)
    print('Failed:',', '.join(failures));raise SystemExit(1)
