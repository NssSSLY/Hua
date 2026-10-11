from pathlib import Path
import subprocess,json,time,ctypes,hashlib,importlib.util,re,shutil
import sys,os
hua,driver,r,build=map(lambda x:Path(x).resolve(),sys.argv[1:])
o=build/'a07-budget';o.mkdir(exist_ok=True);entry=r/'selfhost/bytecode_main.hua';snapshot=o/'current-source';snapshot.mkdir(exist_ok=True)
queue=[entry];seen=set()
while queue:
    p=queue.pop()
    if p.name in seen:continue
    seen.add(p.name)
    text=p.read_text(encoding='utf-8')
    for name in re.findall(r'^import ([A-Za-z_][\w.]*)',text,re.M):
        if not name.startswith('std.'):queue.append(p.parent/(name.replace('.','/')+'.hua'))
for name in sorted(seen):shutil.copy2(entry.parent/name,snapshot/name)
source=snapshot/entry.name
contract=json.loads((r/'selfhost/contract.json').read_text(encoding='utf-8'))
budget=contract['compiler_budget']
assert (budget['execution_steps'],budget['function_calls_depth'],budget['collection_interval'],budget['supervisor_seconds'],budget['report_bytes'])==(500000000,128,65536,600,16777216)
phasefile=o/'phases.log';phasefile.write_text('',encoding='utf-8')
compiler=o/'compiler-source';compiler.mkdir(exist_ok=True)
for name in seen:shutil.copy2(entry.parent/name,compiler/name)
mark='import std.time as time\nimport std.fs as fs\nfn mark(label string) { unwrap(fs.append_text('+json.dumps(phasefile.as_posix())+',label+" "+str(time.monotonic_ns())+"\\n")) }\n'
text=entry.read_text(encoding='utf-8')
text=text.replace('fn main() {',mark+'fn main() {\n    mark("start")',1)
for marker,label in [('let model = unwrap(checked)','analyzed'),('let image = unwrap(emitted)','generated'),('let encoded = report.success','verified'),('print(unwrap(encoded))','encoded')]:
    position=text.index(marker)
    if label in ('analyzed','generated'):position=text.index('\n',position)+1
    text=text[:position]+'mark("'+label+'")\n    '+text[position:]
(compiler/'_timed_emit.hua').write_text(text,encoding='utf-8',newline='\n')
text=(compiler/'bytecode.hua').read_text(encoding='utf-8')
marker='pub fn compile(model types.Model) Result<Image> {'
text=text.replace('pub struct Scalar',mark+'pub struct Scalar',1)
text=text.replace('    let c = lower.lower(model)?','    let c = lower.lower(model)?\n    mark("lowered")',1)
(compiler/'bytecode.hua').write_text(text,encoding='utf-8',newline='\n')
hashes={n:hashlib.sha256((snapshot/n).read_bytes()).hexdigest() for n in sorted(seen)}
inputs=sum((snapshot/n).stat().st_size for n in seen)
print(json.dumps({'stage':'self source captured','modules':len(seen),'input_bytes':inputs}),flush=True)
start=time.monotonic()
p=subprocess.Popen([str(hua),'run',str(compiler/'_timed_emit.hua'),'--execution-profile','compiler','--report-budget','--',str(source)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
class C(ctypes.Structure):
    _fields_=[('cb',ctypes.c_ulong),('faults',ctypes.c_ulong),('peak',ctypes.c_size_t),('working',ctypes.c_size_t),('quotaPeakPaged',ctypes.c_size_t),('quotaPaged',ctypes.c_size_t),('quotaPeakNonPaged',ctypes.c_size_t),('quotaNonPaged',ctypes.c_size_t),('pagefile',ctypes.c_size_t),('peakPagefile',ctypes.c_size_t)]
peak=0 if os.name=='nt' else None;next_update=60;timeout=False
def memory():
    c=C();c.cb=ctypes.sizeof(c)
    if os.name=='nt' and ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(int(p._handle)),ctypes.byref(c),c.cb):return c.peak
    return None
while True:
    measured=memory()
    if measured is not None:peak=max(peak or 0,measured)
    elapsed=time.monotonic()-start
    if elapsed>=next_update:print(json.dumps({'elapsed_seconds':round(elapsed),'peak_working_mib':round(peak/1048576,1) if peak is not None else None}),flush=True);next_update+=60
    remaining=budget['supervisor_seconds']-elapsed
    if remaining<=0:
        p.kill();stdout,stderr=p.communicate();timeout=True;break
    try:
        stdout,stderr=p.communicate(timeout=min(2,remaining));break
    except subprocess.TimeoutExpired:pass
(o/'self.image.json').write_bytes(stdout);(o/'self-budget.json').write_bytes(stderr)
result={'returncode':p.returncode,'supervisor_timeout':timeout,'seconds':round(time.monotonic()-start,3),'peak_working_bytes':peak,'report_bytes':len(stdout),'modules':len(seen),'input_bytes':inputs,'source_hashes':hashes,'execution':'VM compiler profile; isolated full own-source pipeline; '+sys.platform,'emitter':'Hua; not HUAB encoding or generations'}
if p.returncode==0:
    report=json.loads(stdout);result['status']=report['status']
    if report['status']=='ok':
        spec=importlib.util.spec_from_file_location('runner',r/'tools/hua_frontend.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        result['transport_within_budget']=len(stdout)<=contract['compiler_budget']['report_bytes']
        try:m.validate_bytecode(report,contract)
        except (ValueError,AssertionError) as error:result['transport_error']=str(error)
        result.update({'instructions':report['instructions'],'functions':len(report['functions']),'sites':len(report['sites']),'verification':report['verification'],'metrics':json.loads(stderr)})
    else:result['error']=report['error']
(o/'self-scale-results.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
result['phases_ns']={line.split()[0]:int(line.split()[1]) for line in phasefile.read_text().splitlines()}
base=result['phases_ns'].get('start',0);result['phases_ns']={k:v-base for k,v in result['phases_ns'].items()}
(o/'self-scale-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result),flush=True)
if p.returncode!=0 or result.get('status')!='ok' or not result.get('transport_within_budget') or result.get('transport_error'):raise SystemExit(1)
assert result['metrics']['steps']<=500000000 and result['metrics']['peak_calls']<=128
assert result['seconds']<600
# The adapter validates/executes already emitted instructions; it never compiles this input.
calls=[]
def call(args,code=0):
    started=time.monotonic();value=subprocess.run(list(map(str,args)),capture_output=True,encoding='utf-8',timeout=600)
    assert value.returncode==code,(args,value.returncode,value.stderr[:3000])
    calls.append({'action':str(args[1]),'seconds':round(time.monotonic()-started,3),'returncode':value.returncode})
    return value
image=o/'self.image.json'
call([driver,'verify',image])
adapter=o/'self-adapter.huab';call([driver,'build',image,'-o',adapter])
assert snapshot.parent==o and snapshot.name=='current-source'
for name,digest in hashes.items():assert hashlib.sha256((r/'selfhost'/name).read_bytes()).hexdigest()==digest,'source changed during acceptance'
for file in snapshot.glob('*.hua'):file.unlink()
assert not list(snapshot.glob('*.hua'))
sample=r/'examples/frontend_modules/main.hua'
reference=json.loads(call([hua,'run',entry,'--execution-profile','compiler','--',sample]).stdout)
for exe,target in [(driver,image),(hua,adapter)]:
    value=call([exe,'run',target,'--execution-profile','compiler','--',sample])
    assert json.loads(value.stdout)==reference
result['own_instruction_execution']={'paths':'own instructions VM / source-free C++ adapter HUAB','source_removed':True,'adapter_huab_bytes':adapter.stat().st_size,'adapter_huab_sha256':hashlib.sha256(adapter.read_bytes()).hexdigest(),'calls':calls,'container':'C++ test adapter; C07 and continuous generations pending'}
(o/'self-scale-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print('complete current own-source budget and independent instruction execution passed')
