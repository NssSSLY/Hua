"""A07 C04 parity on small inputs only; no own-compiler/generation pipeline."""
from pathlib import Path
import hashlib,json,os,re,shutil,sys,time
from process_guard import run,TestProcessTimeout
exe,root,build=(Path(x).resolve() for x in sys.argv[1:4]);work=build/'a07-c04';work.mkdir(exist_ok=True)
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='900'
cases=json.loads((root/'tests/data/a07_completion_cases.json').read_text(encoding='utf-8'))
excluded={'method_downgrade':'T1 excludes methods','method_refresh':'T1 excludes methods','prior_error_payload':'T1 excludes non-string Result error','unknown_callback_global':'T1 requires annotated parameters'}
selected=[c for c in cases if c['name'] not in excluded]
checks=0;commands=[];rows=[];failures=[];started=time.monotonic()
stamp=str(time.time_ns());log=work/('commands-'+stamp+'.json')
def require(ok,detail):
    global checks
    checks+=1
    if not ok:raise AssertionError(detail)
def call(args,code=0):
    t=time.monotonic();p=run([str(exe),*map(str,args)],capture_output=True,encoding='utf-8',timeout=30)
    commands.append(dict(args=list(map(str,args)),code=p.returncode,seconds=round(time.monotonic()-t,3),stdout=p.stdout,stderr=p.stderr))
    log.write_text(json.dumps(commands,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    require(p.returncode==code,commands[-1]);return p
analyzer=work/'analyzer';analyzer.mkdir(exist_ok=True)
for p in (root/'selfhost').glob('*.hua'):shutil.copyfile(p,analyzer/p.name)
driver=analyzer/'a07_main.hua'
driver.write_text('import types as types\nimport type_report as report\nimport std.os as os\nimport std.path as path\nfn main(){for entry in os.args(){let result=types.analyze(entry)\nlet name=unwrap(path.basename(entry))\nif is_err(result){print(report.failure(name,unwrap_err(result)))}else{print(report.success(unwrap(result),name))}}}\n',encoding='utf-8',newline='\n')
artifact=work/'analyzer.huab';call(['build',driver,'-o',artifact])
inputs=[];native={}
for c in selected:
    d=work/c['name'];d.mkdir(exist_ok=True);source=d/'main.hua';source.write_text(c['source'],encoding='utf-8',newline='\n');inputs.append(source)
    p=call(['check',source],0 if c['expected']=='ok' else 1)
    require(('error['+c['expected']+']') in p.stderr if c['expected']!='ok' else p.stdout=='OK\n',c['name'])
    match=re.search(r' --> .+:(\d+):(\d+)\n',p.stderr)
    native[c['name']]=tuple(map(int,match.groups())) if match else None
reports={mode:{} for mode in ['interpret','run','huab']}
try:
    for offset in range(0,len(selected),4):
        chunk=selected[offset:offset+4];paths=inputs[offset:offset+4]
        for mode in ['interpret','run','huab']:
            entry=artifact if mode=='huab' else driver
            p=call(['run' if mode=='huab' else mode,entry,'--execution-profile','compiler','--',*paths])
            decoded=[json.loads(line) for line in p.stdout.splitlines()]
            require(len(decoded)==len(chunk),(mode,'report count'))
            for c,report in zip(chunk,decoded):reports[mode][c['name']]=report
        for c in chunk:
            name=c['name'];r=reports['run'][name]
            try:
                require(r==reports['interpret'][name]==reports['huab'][name],(name,'AST/VM/HUAB reports differ'))
                require(r.get('status')==('ok' if c['expected']=='ok' else 'error'),(name,r))
                if c['expected']!='ok':
                    match=re.match(r'E3003@(\d+):(\d+)',r.get('error',''));require(match is not None,(name,r))
                    require(tuple(map(int,match.groups()))==native[name],(name,'source position differs',native[name],r))
                rows.append(dict(case=name,status='passed'))
            except AssertionError as error:
                rows.append(dict(case=name,status='failed',error=str(error)));failures.append(name)
        print('C04 small inputs',offset+len(chunk),'/',len(selected),flush=True)
    hidden=work/('hidden-analyzer-'+stamp)
    analyzer.rename(hidden)
    try:
        p=call(['run',artifact,'--execution-profile','compiler','--',inputs[0]])
        require(json.loads(p.stdout)==reports['run'][selected[0]['name']],'source-free analyzer changed report')
    finally:hidden.rename(analyzer)
except BaseException as error:
    failures.append('runner: '+str(error))
    raise
finally:
    result=dict(status='failed' if failures else 'passed',cases=len(rows),checks=checks,commands=len(commands),seconds=round(time.monotonic()-started,3),hua_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),scope='small T1 inputs; AST/VM/HUAB; no compiler own source or generations',excluded=excluded,rows=rows)
    path=work/('results-'+stamp+'.json');path.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    (work/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},ensure_ascii=False),flush=True)
if failures:print('Failed:',', '.join(failures));raise SystemExit(1)
