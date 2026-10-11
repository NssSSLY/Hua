"""Manual fixed-workload C04/C05 performance evidence; never a replacement for regression gates.
Usage: script hua repo build frozen-input-directory compiler-directory label [--summary]
Captures time, peak working set, steps, exact report and fixed input hashes under build/c04-performance.
"""
from pathlib import Path
import subprocess,sys,json,time,ctypes,os,hashlib,re,shutil,importlib.util
hua,repo,build,source,compiler=map(lambda p:Path(p).resolve(),sys.argv[1:6]);label=sys.argv[6]
assert re.fullmatch(r'[a-zA-Z0-9_-]+',label)
assert sys.argv[7:] in ([],['--summary'])
summary=sys.argv[7:]==['--summary']
out=build/'c04-performance';out.mkdir(exist_ok=True)
contract=json.loads((repo/'selfhost/contract.json').read_text(encoding='utf-8'));limits=contract['compiler_budget']
contract['budgets']=limits['parser'];contract['analysis']['budgets']=limits['analysis']
assert (limits['execution_steps'],limits['function_calls_depth'],limits['collection_interval'],limits['supervisor_seconds'],limits['report_bytes'])==(500000000,128,65536,600,16777216)
entry=source/'bytecode_main.hua';pending=[entry];files={}
while pending:
    file=pending.pop()
    if file.name in files:continue
    data=file.read_bytes();files[file.name]=hashlib.sha256(data).hexdigest()
    for name in re.findall(r'^import ([A-Za-z_][\w.]*)',data.decode('utf-8'),re.M):
        if not name.startswith('std.'):pending.append(file.parent/(name.replace('.','/')+'.hua'))
work=out/(label+'-compiler');work.mkdir(exist_ok=True)
for file in compiler.glob('*.hua'):shutil.copy2(file,work/file.name)
phase=out/(label+'-phases.log');phase.write_bytes(b'')
mark='import std.time as perf_time\nimport std.fs as perf_fs\nimport std.gc as perf_gc\nfn perf_mark(label string) { unwrap(perf_fs.append_text('+json.dumps(phase.as_posix())+',label+" "+str(perf_time.monotonic_ns())+" "+str(perf_gc.allocated())+"\\n")) }\n'
def inject(file,anchor,edits):
    data=file.read_text(encoding='utf-8');data=data.replace(anchor,mark+anchor,1)
    for old,new in edits:
        assert old in data,(file,old)
        data=data.replace(old,new,1)
    file.write_text(data,encoding='utf-8',newline='\n')
inject(work/'lowering_main.hua','fn main() {',[
    ('fn main() {','fn main() {\n    perf_mark("start")'),
    ('let model = unwrap(checked)','let model = unwrap(checked)\n    perf_mark("analyzed")'),
    ('let encoded = report.success','perf_mark("lowered")\n    let encoded = report.success'),
    ('print(unwrap(encoded))','perf_mark("encoded")\n    print(unwrap(encoded))')])
inject(work/'types.hua','pub struct Info',[
    ('let graph = modules.analyze_compiler(entry)?','let graph = modules.analyze_compiler(entry)?\n    perf_mark("names")')])
if summary:
    file=work/'lowering_main.hua';data=file.read_text(encoding='utf-8')
    data=data.replace('fn perf_mark(label','import std.json as perf_json\nimport std.list as perf_list\nfn perf_mark(label',1)
    data=data.replace('fn main() {','struct Summary { schema string\n status string\n nodes int\n work int\n facts int\n symbols int\n functions int }\nfn main() {',1)
    a=data.index('    let encoded = report.success');data=data[:a]+"    let core = unwrap(result)\n    print(unwrap(perf_json.encode(Summary{schema:\"hua.pipeline.summary.v1\",status:\"ok\",nodes:perf_list.len(core.nodes),work:core.work,facts:len(model.facts),symbols:len(model.symbols),functions:len(model.functions)})))\n}\n"
    file.write_text(data,encoding='utf-8',newline='\n')
args=[str(hua),'run',str(work/'lowering_main.hua'),'--execution-profile','compiler','--report-budget','--',str(entry)]
start=time.monotonic();p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
class Memory(ctypes.Structure):
    _fields_=[('cb',ctypes.c_ulong),('faults',ctypes.c_ulong),('peak',ctypes.c_size_t),('working',ctypes.c_size_t),('qpp',ctypes.c_size_t),('qp',ctypes.c_size_t),('qpnp',ctypes.c_size_t),('qnp',ctypes.c_size_t),('pagefile',ctypes.c_size_t),('peakfile',ctypes.c_size_t)]
peak=0 if os.name=='nt' else None;timeout=False;next_update=60
while True:
    m=Memory();m.cb=ctypes.sizeof(m)
    if os.name=='nt' and ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(int(p._handle)),ctypes.byref(m),m.cb):peak=max(peak or 0,m.peak)
    elapsed=time.monotonic()-start
    if elapsed>=next_update:print(json.dumps({'label':label,'elapsed_seconds':round(elapsed),'peak_mib':round(peak/1048576,1) if peak is not None else None}),flush=True);next_update+=60
    remaining=limits['supervisor_seconds']-elapsed
    if remaining<=0:p.kill();stdout,stderr=p.communicate();timeout=True;break
    try:stdout,stderr=p.communicate(timeout=min(2,remaining));break
    except subprocess.TimeoutExpired:pass
(out/(label+'.core.json')).write_bytes(stdout);(out/(label+'-budget.json')).write_bytes(stderr)
phase_rows=[line.split() for line in phase.read_text().splitlines()];phases={row[0]:int(row[1]) for row in phase_rows};base=phases.get('start',0)
allocations={row[0]:int(row[2]) for row in phase_rows if len(row)==3}
result={'label':label,'output':'pipeline-summary' if summary else 'core-report','managed_allocations':allocations,'returncode':p.returncode,'supervisor_timeout':timeout,'seconds':round(time.monotonic()-start,3),'peak_working_bytes':peak,'report_bytes':len(stdout),'report_sha256':hashlib.sha256(stdout).hexdigest(),'modules':len(files),'input_bytes':sum((source/name).stat().st_size for name in files),'source_hashes':files,'phases_seconds':{name:round((ns-base)/1e9,6) for name,ns in phases.items()},'execution':'isolated Windows VM compiler profile' if os.name=='nt' else sys.platform+' VM compiler profile'}
(out/(label+'-results.json')).write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
if p.returncode==0:
    report=json.loads(stdout);result['status']=report['status']
    result['metrics']=json.loads(stderr)
    assert result['metrics']['profile']=='compiler' and result['metrics']['steps']<=limits['execution_steps'] and result['metrics']['peak_calls']<=limits['function_calls_depth']
    if result['status']=='ok':
        spec=importlib.util.spec_from_file_location('frontend',repo/'tools/hua_frontend.py');runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
        try:
            if summary:
                assert report['schema']=='hua.pipeline.summary.v1' and all(type(report[k]) is int and report[k]>=0 for k in ['nodes','work','facts','symbols','functions'])
                assert report['nodes']<=contract['lowering']['nodes'] and report['work']<=contract['lowering']['work']
            else:runner.validate_lowering(report,contract)
            assert len(stdout)<=limits['report_bytes']
            result['validated']=True
        except (ValueError,AssertionError) as error:result['transport_error']=str(error);result['validated']=False
        result['core_nodes']=report['nodes'] if summary else len(report['nodes'])
    else:result['error']=report['error']
assert all(hashlib.sha256((source/name).read_bytes()).hexdigest()==digest for name,digest in files.items()),'frozen input changed'
(out/(label+'-results.json')).write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k!='source_hashes'}),flush=True)
if p.returncode!=0 or result.get('status')!='ok' or not result.get('validated'):raise SystemExit(1)
