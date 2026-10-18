from process_guard import run as run_process
"""A02: fixed type boundaries, independent analyzers, two engines and source-free HUAB."""
from pathlib import Path
import atexit,base64,ctypes,hashlib,importlib.util,json,re,shutil,struct,subprocess,sys,time,zlib
hua,probe,oracle,driver,repo,build=map(lambda p:Path(p).resolve(),sys.argv[1:7])
native_only='--native-only' in sys.argv[7:]
cases_only='--cases-only' in sys.argv[7:]
assert not cases_only or native_only,'--cases-only requires --native-only'
# Release the temporary thread request on exit; keep long gates out of Modern Standby.
if sys.platform=='win32':
    previous=ctypes.windll.kernel32.SetThreadExecutionState(0x80000001)
    if previous:atexit.register(ctypes.windll.kernel32.SetThreadExecutionState,previous)
work=build/'a02/acceptance';work.mkdir(parents=True,exist_ok=True)
checks=0;rows=[];started=time.monotonic()
def require(condition,details):
    global checks
    assert condition,details
    checks+=1
def call(args,code=0):
    p=run_process(list(map(str,args)),capture_output=True,encoding='utf-8',timeout=600)
    require(p.returncode==code,(args,p.returncode,p.stdout[:1000],p.stderr[:1500]))
    return p
def diagnostic(stderr):
    m=re.search(r'error\[(E\d+)\].*?\n --> .*?:(\d+):(\d+)\n',stderr,re.S)
    require(m is not None,stderr)
    return m[1],int(m[2]),int(m[3])
cases=json.loads((repo/'tests/data/a02_cases.json').read_text(encoding='utf-8'))
compiler_paths=[]
if not native_only:
    persisted=work/'persisted';persisted.mkdir(exist_ok=True)
    for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,persisted/p.name)
    artifact=persisted/'types.huab';call([probe,'build',persisted/'types_main.hua','-o',artifact])
    require(struct.unpack_from('<II',artifact.read_bytes(),8)==(7,1),'compiler archive version')
    for p in persisted.glob('*.hua'):p.unlink()
    require(not list(persisted.glob('*.hua')),'compiler sources absent')
    compiler_paths=[(hua,'interpret',repo/'selfhost/types_main.hua'),(hua,'run',repo/'selfhost/types_main.hua'),(hua,'run',artifact),(probe,'run',repo/'selfhost/types_main.hua')]
emit_cases={'array_identity','array_binding','array_decimal_extent','array_nested','array_map','array_mut_borrow','array_result','optional_context','result_optional_context','result_nested_void','multi_forward','array_zero'}
spec=importlib.util.spec_from_file_location('a02_runner',repo/'tools/hua_frontend.py');runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
contract=json.loads((repo/'selfhost/contract.json').read_text(encoding='utf-8'))
for c in cases:
    d=work/c['name'];d.mkdir(exist_ok=True);source=d/'input.hua';source.write_text(c['source'],encoding='utf-8',newline='\n')
    artifact=d/'input.huab';artifact.unlink(missing_ok=True)
    check=call([hua,'check',source],1 if c['phase']=='check' else 0)
    if c['phase']=='check':
        location=diagnostic(check.stderr);require(location[0]==c['want'] and not check.stdout,(c,check))
        for action in ('interpret','run','build'):
            args=[hua,action,source]+(['-o',artifact] if action=='build' else [])
            p=call(args,1);require(diagnostic(p.stderr)==location and not p.stdout,(c,p))
        require(not artifact.exists(),'static failure published artifact')
    else:
        wanted_code=1 if c['phase']=='run' else 0
        results=[call([hua,action,source],wanted_code) for action in ('interpret','run')]
        call([hua,'build',source,'-o',artifact]);hidden=source.with_suffix('.removed');source.rename(hidden)
        try:
            require(not source.exists(),'program source must be absent')
            results.append(call([hua,'run',artifact],wanted_code))
        finally:hidden.rename(source)
        for p in results:
            require(p.stdout==(c.get('stdout','') if c['phase']=='run' else c['want']),(c['name'],p.stdout,c))
        if c['phase']=='run':
            first=diagnostic(results[0].stderr);require(first[0]==c['want'],(c,first))
            for p in results[1:]:require(diagnostic(p.stderr)==first,(c,p))
    if c['analyzer'] and compiler_paths:
        reports=[]
        for exe,action,target in compiler_paths:
            p=call([exe,action,target,'--execution-profile','compiler','--',source]);reports.append(json.loads(p.stdout))
        require(all(r==reports[0] for r in reports),(c['name'],'analyzer path disagreement'))
        ref=json.loads(call([oracle,'--types',source]).stdout)
        require(reports[0]['status']==ref['status'],(c['name'],reports[0],ref))
        if ref['status']=='error':
            err=reports[0]['error'];require(err.startswith(f"{ref['code']}@{ref['line']}:{ref['column']}"),(c['name'],err,ref))
            require(not any(k in reports[0] for k in ('facts','functions','symbols','graph')),'partial facts on failure')
        else:
            facts={}
            for f in ref['facts']:facts.setdefault((f['module'],f['start'],f['end']),[]).append(f)
            for f in reports[0]['facts']:
                options=facts.get((f['module'],f['start'],f['end']),[])
                if options:require(any(f['type_name']==x['type_name'] and (x['writable'] is None or f['writable']==x['writable']) for x in options),(c['name'],f,options))
    if c['name'] in emit_cases and not native_only:
        p=call([hua,'run',repo/'selfhost/bytecode_main.hua','--execution-profile','compiler','--',source]);report=json.loads(p.stdout)
        require(report['status']=='ok',(c['name'],report));runner.validate_bytecode(report,contract)
        image=d/'instructions.json';image.write_text(p.stdout,encoding='utf-8');require(call([driver,'run',image]).stdout==c['want'],c['name'])
        # Container bytes come from Hua, and the independent C++ reader executes them.
        p=call([hua,'run',repo/'selfhost/archive_main.hua','--execution-profile','compiler','--',source,d/'hua.huab'])
        require(json.loads(p.stdout)['status']=='ok',(c['name'],p.stdout))
        source.rename(source.with_suffix('.removed'))
        try:require(call([hua,'run',d/'hua.huab']).stdout==c['want'],c['name'])
        finally:source.with_suffix('.removed').rename(source)
    rows.append({'case':c['name'],'phase':c['phase'],'analyzer':c['analyzer'] and not native_only,'Hua_encoding':c['name'] in emit_cases and not native_only})
    print('A02',c['name'],'passed',flush=True)
for old in json.loads((repo/'tests/data/a02_legacy_arrays.json').read_text(encoding='utf-8')):
    data=base64.b64decode(old['base64']);require(hashlib.sha256(data).hexdigest()==old['sha256'],'old fixture hash')
    require(struct.unpack_from('<II',data,8)==(6,1),'old format');require(not (repo/'a02-legacy-removed/input.hua').exists(),'old source absent');p=work/(old['name']+'-old.huab');p.write_bytes(data)
    require(call([hua,'run',p]).stdout==old['stdout'],'old type identity compatibility')
    marker=b'[0_2]' if b'[0_2]' in data else b'[02]';require(marker in data,'old raw spelling')
    for label,bad in [('letters',b'['+b'x'*(len(marker)-2)+b']'),('separator',b'['+b'_'*(len(marker)-2)+b']')]:
        payload=data[32:].replace(marker,bad);corrupt=data[:24]+struct.pack('<I',zlib.crc32(payload))+data[28:32]+payload
        p=work/(old['name']+'-'+label+'.huab');p.write_bytes(corrupt)
        result=call([hua,'run',p],1);require('[E6002]' in result.stderr and not result.stdout,'malformed type archive')
# All current compiler units still pass the native front-end. This is not a self-build budget gate.
compiler_units=[] if cases_only else sorted((repo/'selfhost').glob('*.hua'))
for source in compiler_units:call([hua,'check',source])
summary={'cases':len(rows),'checks':checks,'analyzer_cases':sum(x['analyzer'] for x in rows),'Hua_encoded_cases':sum(x['Hua_encoding'] for x in rows),'compiler_units':len(compiler_units),'scope':'VM type cases and legacy HUAB only' if cases_only else 'full selected type contract','seconds':round(time.monotonic()-started,3),'rows':rows}
(work/('native-results.json' if native_only else 'results.json')).write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps({k:v for k,v in summary.items() if k!='rows'},ensure_ascii=False),flush=True)
