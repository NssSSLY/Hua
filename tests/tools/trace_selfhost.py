from pathlib import Path
import ctypes as c, ctypes.wintypes as w, hashlib, json, os, subprocess, sys, time
ROOT=Path(r'C:\file\project\hua')
OUT=Path(sys.argv[1]).resolve() if len(sys.argv)==2 else ROOT/'tests/artifacts/selfhost-trace-20261020-run'
sys.path.insert(0,str(ROOT/'tests/tools'))
from audit_bootstrap_phases import closure, machine
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,v): Path(p).write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
class Mem(c.Structure):
 _fields_=[('cb',w.DWORD),('PageFaultCount',w.DWORD)]+[(n,c.c_size_t) for n in ('PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage')]
K=c.WinDLL('kernel32',use_last_error=True)
K.CreateJobObjectW.argtypes=[c.c_void_p,w.LPCWSTR];K.CreateJobObjectW.restype=w.HANDLE
K.AssignProcessToJobObject.argtypes=[w.HANDLE,w.HANDLE];K.AssignProcessToJobObject.restype=w.BOOL
K.TerminateJobObject.argtypes=[w.HANDLE,w.UINT];K.TerminateJobObject.restype=w.BOOL
K.CloseHandle.argtypes=[w.HANDLE]
K.GetProcessTimes.argtypes=[w.HANDLE]+[c.POINTER(w.FILETIME)]*4
P=c.WinDLL('psapi',use_last_error=True)
P.GetProcessMemoryInfo.argtypes=[w.HANDLE,c.POINTER(Mem),w.DWORD]
def stats(p):
 f=[w.FILETIME() for _ in range(4)];cpu=None
 if K.GetProcessTimes(w.HANDLE(int(p._handle)),*[c.byref(x) for x in f]):cpu=sum((x.dwHighDateTime<<32)|x.dwLowDateTime for x in f[2:])/1e7
 m=Mem();m.cb=c.sizeof(m);ok=P.GetProcessMemoryInfo(w.HANDLE(int(p._handle)),c.byref(m),m.cb)
 return {'cpu_seconds':cpu,'working_bytes':m.WorkingSetSize if ok else None,'peak_working_bytes':m.PeakWorkingSetSize if ok else None,'private_bytes':m.PagefileUsage if ok else None}
def main():
 manifest=json.loads((OUT/'manifest.json').read_text(encoding='utf-8'))
 assert not (OUT/'result.json').exists() and not (OUT/'events.jsonl').exists()
 sources=closure(ROOT/'selfhost/archive_main.hua')
 assert {n:sha(p) for n,p in sources.items()}=={n:v['sha256'] for n,v in manifest['source_files'].items()}
 observer=OUT/'trace-selfhost.exe'
 c0=Path(manifest['compiler']['path'])
 assert sha(c0)==manifest['compiler']['sha256']
 args=[str(observer),str(c0),str(ROOT/'selfhost/archive_main.hua'),str(OUT/'compiler1.huab'),str(OUT/'events.jsonl'),str(OUT/'trace-summary.json')]
 manifest['command']=args;manifest['observer_binary_sha256']=sha(observer)
 manifest['observer_source_sha256']=sha(ROOT/'tests/tools/trace_selfhost.cpp')
 save(OUT/'manifest.json',manifest)
 job=K.CreateJobObjectW(None,None)
 if not job:raise c.WinError(c.get_last_error())
 with (OUT/'stdout.txt').open('wb') as stdout,(OUT/'stderr.txt').open('wb') as stderr,(OUT/'progress.jsonl').open('w',encoding='utf-8') as progress:
  start=time.monotonic()
  p=subprocess.Popen(args,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
  if not K.AssignProcessToJobObject(job,w.HANDLE(int(p._handle))):
   error=c.get_last_error();p.kill();p.wait();raise OSError(error,'cannot contain compiler process')
  save(OUT/'live.json',{'pid':p.pid,'started_monotonic':start,'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'deadline_seconds':600,'command':args})
  print(json.dumps({'event':'started','pid':p.pid,'limit':600,'out':str(OUT)}),flush=True)
  next_log=0;timeout=False;at_stop=None
  while p.poll() is None:
   elapsed=time.monotonic()-start
   if (OUT/'stdout.txt').stat().st_size+(OUT/'stderr.txt').stat().st_size>16777216:
    at_stop=stats(p);K.TerminateJobObject(job,125);break
   if elapsed>=600:
    timeout=True;at_stop=stats(p)
    if not K.TerminateJobObject(job,124):p.kill()
    break
   if elapsed>=next_log:
    events=(OUT/'events.jsonl').read_text(encoding='utf-8').splitlines() if (OUT/'events.jsonl').exists() else []
    latest=json.loads(events[-1]) if events and events[-1].endswith('}') else None
    row={'last_event':latest,'seconds':elapsed,**stats(p),'stdout_bytes':(OUT/'stdout.txt').stat().st_size,'stderr_bytes':(OUT/'stderr.txt').stat().st_size,'artifact_exists':(OUT/'compiler1.huab').exists()}
    progress.write(json.dumps(row)+'\n');progress.flush();print(json.dumps({'event':'progress',**row}),flush=True);next_log+=30
   try:p.wait(timeout=min(0.25,max(0.001,600-(time.monotonic()-start))))
   except subprocess.TimeoutExpired:pass
  p.wait(timeout=10);elapsed=time.monotonic()-start
  result={'schema':'hua.bootstrap.formal-result.v1','status':'timeout' if timeout else ('process-ok' if p.returncode==0 else 'process-failed'),'timeout':timeout,'seconds':elapsed,'returncode':p.returncode,'pid':p.pid,**(at_stop or stats(p)),'artifact_exists':(OUT/'compiler1.huab').exists(),'attempts':1,'automatic_retry':False}
 K.CloseHandle(job)
 result['stdout_bytes']=(OUT/'stdout.txt').stat().st_size;result['stderr_bytes']=(OUT/'stderr.txt').stat().st_size
 result['sources_unchanged']=all(sha(p)==manifest['source_files'][n]['sha256'] for n,p in sources.items())
 if result['artifact_exists']:result['artifact_sha256']=sha(OUT/'compiler1.huab');result['artifact_bytes']=(OUT/'compiler1.huab').stat().st_size
 save(OUT/'result.json',result);print(json.dumps({'event':'finished',**result}),flush=True)
 return 0

if __name__=='__main__':raise SystemExit(main())
