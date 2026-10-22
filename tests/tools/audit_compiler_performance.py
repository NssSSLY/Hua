from pathlib import Path
import argparse, ctypes, ctypes.wintypes as w, hashlib, json, os, signal, subprocess, sys, time
options=argparse.ArgumentParser(description='Single-pass compiler diagnostic; stop at first timeout, never retry.')
options.add_argument('--hua',type=Path,required=True)
options.add_argument('--output',type=Path,required=True)
options.add_argument('--sizes',type=int,nargs='+',default=[10,100,500,1000])
options.add_argument('--unbounded',action='store_true',help='Disable the 30s test stop; preserve the 600s single compiler budget')
args=options.parse_args()
if any(n<1 or n>1000 for n in args.sizes):options.error('sizes must be in 1..1000')
root=Path(__file__).resolve().parents[2];out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
if (out/'results.json').exists():options.error('output already contains evidence; choose a new directory')
hua=args.hua.resolve()
options_unbounded=args.unbounded
rows=[];started=time.monotonic();stopped=False
class Memory(ctypes.Structure):
 _fields_=[('cb',w.DWORD),('PageFaultCount',w.DWORD)]+[(n,ctypes.c_size_t) for n in ('PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage')]
def record(label,args,limit=30):
 global stopped
 if stopped:return None
 if options_unbounded:limit=600
 args=list(map(str,args));t=time.monotonic();p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,**({'start_new_session':True} if os.name!='nt' else {}));timed=False
 try:stdout,stderr=p.communicate(timeout=limit)
 except subprocess.TimeoutExpired:
  timed=True
  if os.name=='nt':
   try:subprocess.run(['taskkill','/F','/T','/PID',str(p.pid)],capture_output=True,timeout=5)
   except (OSError,subprocess.TimeoutExpired):pass
  else:os.killpg(p.pid,signal.SIGKILL)
  if p.poll() is None:p.kill()
  stdout,stderr=p.communicate();stopped=True
 wall=time.monotonic()-t
 ft=[w.FILETIME() for _ in range(4)];cpu=None
 if os.name=='nt' and ctypes.windll.kernel32.GetProcessTimes(w.HANDLE(int(p._handle)),*[ctypes.byref(f) for f in ft]):cpu=sum((f.dwHighDateTime<<32)|f.dwLowDateTime for f in ft[2:])/10000000
 mem=Memory();mem.cb=ctypes.sizeof(mem);peak=None
 if os.name=='nt' and ctypes.windll.psapi.GetProcessMemoryInfo(w.HANDLE(int(p._handle)),ctypes.byref(mem),mem.cb):peak=mem.PeakWorkingSetSize
 row={'label':label,'status':'timeout' if timed else 'passed' if p.returncode==0 else 'failed','command':args,'limit_seconds':limit,'wall_seconds':round(wall,6),'cpu_seconds':cpu,'peak_working_set_bytes':peak,'code':p.returncode,'stdout_bytes':len(stdout),'stderr_bytes':len(stderr)}
 (out/(label+'.stdout')).write_bytes(stdout);(out/(label+'.stderr')).write_bytes(stderr)
 try:
  v=json.loads(stdout);row['report_status']=v.get('status');row['report_stage']=v.get('stage');row['report_error']=v.get('error')
 except (ValueError,AttributeError):row['stdout_preview']=stdout[:200].decode('utf-8',errors='replace')
 try:row['budget']=json.loads(stderr)
 except ValueError:pass
 if row.get('report_status')=='error':row['status']='failed'
 rows.append(row);save();print(json.dumps(row,ensure_ascii=False),flush=True)
 if timed:print('STOP: first severe timeout; no retries or remaining commands',flush=True)
 if (p.returncode or row.get('report_status')=='error') and not timed:stopped=True;save()
 return row
def save():
 (out/'results.json').write_text(json.dumps({'status':'stopped' if stopped else 'running','rows':rows,'seconds':round(time.monotonic()-started,3),'retry':False,'unbounded_tests':options_unbounded,'compiler_wall_limit_seconds':600,'runtime_sha256':hashlib.sha256(hua.read_bytes()).hexdigest(),'compiler_source_hashes':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'selfhost').glob('*.hua')},'input_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('workload*.hua')}},ensure_ascii=False,indent=2),encoding='utf-8')
# Cheap mechanism probes; allocated is counted inside the Hua process (not inferred).
for n in (100,1000):
 source=out/f'workload-value-{n}.hua'
 source.write_text('import std.gc as g\nstruct Box{x int\ny int}\nfn make(x int) Result<Box>{return ok(Box{x:x,y:1})}\nfn main(){let before=g.allocated()\nvar total=0\nfor i in 0..'+str(n)+'{let box=unwrap(make(i))\ntotal+=box.x}\nprint(total,g.allocated()-before)}\n',encoding='utf-8')
 # The preceding text contains intentional newlines, not language string escapes.
 record(f'value_{n}',[hua,'run',source,'--execution-profile','compiler','--report-budget'])
 if stopped:sys.exit(1)
# Same input, native compiler and compiler written in Hua; each invocation runs only once.
for n in args.sizes:
 source=out/f'workload-{n}.hua';source.write_text('fn main(){\n'+''.join(f'let x{i}={i}\n' for i in range(n))+f'print(x{n-1})}}\n',encoding='utf-8')
 record(f'native_check_{n}',[hua,'check',source])
 if stopped:break
 record(f'parser_{n}',[hua,'run',root/'selfhost/parser_compiler_main.hua','--execution-profile','compiler','--report-budget','--',source,'input.hua'])
 if stopped:break
 record(f'types_{n}',[hua,'run',root/'selfhost/types_main.hua','--execution-profile','compiler','--report-budget','--',source])
 if stopped:break
 record(f'bytecode_{n}',[hua,'run',root/'selfhost/bytecode_main.hua','--execution-profile','compiler','--report-budget','--',source])
 if stopped:break
if not stopped:
 data=json.loads((out/'results.json').read_text(encoding='utf-8'));data['status']='passed';(out/'results.json').write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
sys.exit(1 if stopped else 0)
