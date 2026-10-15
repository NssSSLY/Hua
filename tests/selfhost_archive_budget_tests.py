# Manual isolated C07 full own-source workload; no C++ archive encoder in this path.
from pathlib import Path
import subprocess,json,time,ctypes,hashlib,re,shutil,sys,os,atexit
hua,repo,build=map(lambda p:Path(p).resolve(),sys.argv[1:4]);options=set(sys.argv[4:]);assert options <= {'--bootstrap-huab','--diagnostic-phases','--diagnostic-helpers'};bootstrap='--bootstrap-huab' in options;diagnostic='--diagnostic-phases' in options;helpers='--diagnostic-helpers' in options;assert not helpers or diagnostic
o=build/('c07-budget-bootstrap' if bootstrap else 'c07-budget');o.mkdir(exist_ok=True)
# Windows Modern Standby suspends both compiler and supervisor and invalidates wall-time measurements.
# This is a thread-scoped idle-sleep request, not a change to the power plan or the 600-second limit.
awake=False
if os.name=='nt':
 state=ctypes.windll.kernel32.SetThreadExecutionState(0x80000001)
 assert state,'cannot establish temporary idle-sleep prevention for isolated budget measurement'
 atexit.register(ctypes.windll.kernel32.SetThreadExecutionState,state);awake=True
entry=repo/'selfhost/archive_main.hua';snapshot=o/'current-source';compiler=o/'compiler-source'
snapshot.mkdir(exist_ok=True);compiler.mkdir(exist_ok=True);seen=set();queue=[entry]
while queue:
 p=queue.pop()
 if p.name in seen:continue
 seen.add(p.name)
 for name in re.findall(r'^import ([A-Za-z_][\w.]*)',p.read_text(encoding='utf-8'),re.M):
  if not name.startswith('std.'):queue.append(p.parent/(name.replace('.','/')+'.hua'))
for name in seen:
 shutil.copy2(entry.parent/name,snapshot/name);shutil.copy2(entry.parent/name,compiler/name)
source=snapshot/entry.name;contract=json.loads((repo/'selfhost/contract.json').read_bytes());b=contract['compiler_budget']
assert (b['execution_steps'],b['function_calls_depth'],b['collection_interval'],b['supervisor_seconds'],b['report_bytes'])==(500000000,128,65536,600,16777216)
hashes={name:hashlib.sha256((snapshot/name).read_bytes()).hexdigest() for name in sorted(seen)}
phasefile=o/'phases.log';phasefile.write_text('',encoding='utf-8')
if diagnostic:
 mark='fn mark(label string) { unwrap(fs.append_text('+json.dumps(phasefile.as_posix())+',label+" "+str(time.monotonic_ns())+"\\n")) }\n'
 def instrument(text, add_fs=False):
  imports=list(re.finditer(r'^import [^\n]*\n',text,re.M));assert imports
  point=imports[-1].end()
  extra=('import std.fs as fs\n' if add_fs else '')+'import std.time as time\n'
  return text[:point]+extra+mark+text[point:]
 text=instrument(entry.read_text(encoding='utf-8'),True).replace('fn main() {','fn main() {\n    mark("start")',1)
 for marker,label in [('    let emitted=bc.compile','analyzed'),('    let encoded=archive.encode','generated'),('    let receipt=unwrap(json.encode','encoded'),('    print(receipt)','published')]:
  assert text.count(marker)==1;text=text.replace(marker,'    mark("'+label+'")\n'+marker,1)
 (compiler/'_timed_archive.hua').write_text(text,encoding='utf-8',newline='\n')
 for module,marker,label in [('bytecode.hua','    let c = lower.lower(model)?','lowered'),('archive.hua','    verify.verify(image)?','verified')]:
  p=compiler/module;text=instrument(p.read_text(encoding='utf-8'),module=='bytecode.hua');assert text.count(marker)==1;text=text.replace(marker,marker+'\n    mark("'+label+'")',1);p.write_text(text,encoding='utf-8',newline='\n')
 # Instrument only the copied type analyzer, preserving every production source hash.
 p=compiler/'types.hua';text=instrument(p.read_text(encoding='utf-8'),True)
 for marker,label in [('    let graph = modules.analyze_compiler(entry)?','resolved'),('    infer(m,units)?','inferred')]:
  assert text.count(marker)==1;text=text.replace(marker,marker+'\n    mark("'+label+'")',1)
 text=text.replace('    infer(m,units)?','    mark("signatures")\n    infer(m,units)?',1)
 marker='    let capabilities=flow.analyze(graph,m.refs,m.declarations,m.fields,flow_functions,flow_variants)?';assert text.count(marker)==1
 text=text.replace(marker,'    mark("members-start")\n'+marker+'\n    mark("members-finished")',1)
 marker='        m.capability_passes += 1';assert text.count(marker)==1
 text=text.replace(marker,marker+'\n        mark("return-pass:"+str(m.capability_passes))',1)
 marker='    for name in list.snapshot(graph.order) {'
 assert text.count(marker)==2
 parts=text.rsplit(marker,1);text=parts[0]+'    mark("globals")\n'+marker+'\n        mark("types:"+name)'+parts[1]
 p.write_text(text,encoding='utf-8',newline='\n')
 p=compiler/'capabilities.hua';text=instrument(p.read_text(encoding='utf-8'),True)
 if helpers:
  marker='pub struct View {';assert text.count(marker)==1
  observer='var _profile_on=false\nvar _profile_ns=map[string]int{}\nvar _profile_calls=map[string]int{}\nfn _profile_record(name string,elapsed int){_profile_ns[name]=unwrap_or(_profile_ns[name],0)+elapsed\n_profile_calls[name]=unwrap_or(_profile_calls[name],0)+1}\nfn _profile_dump(target string){for name,elapsed in _profile_ns{mark("helper:"+target+":"+name+":ns:"+str(elapsed)+":calls:"+str(unwrap(_profile_calls[name])))}}\n'
  text=text.replace(marker,observer+marker,1)
  wrappers=[('select','input Value, path string, type string','input,path,type','Value'),('select_cached','e mut Environment, input Value, path string, type string, owner string, version int','e,input,path,type,owner,version','Value'),('merge','a Value, b Value','a,b','Value'),('same_value','a Value, b Value','a,b','bool'),('instantiate','caller string, module string, node int, source Value, actual map[string]Value','caller,module,node,source,actual','Value'),('assign','refs map[string]string, e mut Environment, left ast.Node, input Value','refs,e,left,input','Result<void>'),('put','e mut Environment, path string, input Value, weak bool, copy bool','e,path,input,weak,copy',''),('field_defaults','s mut State, base View, level int','s,base,level','map[string]View'),('seeded','s mut State, type string, writable bool, owner string','s,type,writable,owner','Value'),('snapshot','values map[string]Value','values','map[string]Value'),('invoke','s mut State, e mut Environment, n ast.Node, f Function, args []Value','s,e,n,f,args','Result<Value>')]
  for name,parameters,arguments,result in wrappers:
   marker='fn '+name+'(';assert text.count(marker)==1
   raw='_profile_raw_'+name;signature='fn '+name+'('+parameters+')'+(' '+result if result else '')+' {'
   call=raw+'('+arguments+')'
   inactive=('return '+call) if result else (call+'\nreturn')
   active=('var result='+call) if result else call
   wrapper=signature+'\nif !_profile_on{'+inactive+'}\nlet start=time.monotonic_ns()\n'+active+'\n_profile_record("'+name+'",time.monotonic_ns()-start)\n'+('return result\n' if result else '')+'}\n'
   text=text.replace(marker,wrapper+'fn '+raw+'(',1)
  marker='            statement(s,e,n.children[1])?';assert text.count(marker)>=1
  before='            let _profile_this=passes==1 && (f.target=="capabilities.hua#expression_body" || f.target=="types.hua#statement")\n            if _profile_this{_profile_ns=map[string]int{}\n_profile_calls=map[string]int{}\n_profile_on=true}\n'
  after='\n            if _profile_this{_profile_on=false\n_profile_dump(f.target)}'
  text=text.replace(marker,before+marker+after,1)
 marker='    while changed && passes<32 { passes+=1';assert text.count(marker)==1
 text=text.replace(marker,marker+'\n        mark("member-pass:"+str(passes)+":work:"+str(s.work))',1)
 marker='            var e=environment(unwrap(s.units[f.module]),name,globals)';assert text.count(marker)>=1
 text=text.replace(marker,'            mark("member-function:"+str(passes)+":"+name+":work:"+str(s.work))\n'+marker,1)
 marker='        while changed && passes<32 {\n            passes+=1';assert text.count(marker)==1
 text=text.replace(marker,marker+'\n            if e.function=="capabilities.hua#expression_body" || e.function=="types.hua#statement" { mark("flow-loop:"+e.function+":"+str(n.span.line)+":"+str(passes)+":work:"+str(s.work)) }',1)
 marker='    s.probing=false';assert text.count(marker)==1
 text=text.replace(marker,'    mark("member-validation:work:"+str(s.work))\n'+marker,1)
 p.write_text(text,encoding='utf-8',newline='\n')
 p=compiler/'modules.hua';text=instrument(p.read_text(encoding='utf-8'))
 for marker,prefix,label in [('    var parsed Result<parser.Tree> = err("uninitialized")',True,'parse:'),('    let unit = Unit{id:id,path:normalized,key:key,tree:unwrap(parsed)}',True,'parsed:'),('    for id in list.snapshot(g.order) {',False,'names:')]:
  assert text.count(marker)==1
  if prefix:text=text.replace(marker,'    mark("'+label+'"+id)\n'+marker,1)
  else:text=text.replace(marker,marker+'\n        mark("'+label+'"+id)',1)
 marker='    for u in list.snapshot(g.units) { collect(g,u)? }';assert text.count(marker)==1
 text=text.replace(marker,'    mark("loaded")\n'+marker+'\n    mark("collected")',1)
 p.write_text(text,encoding='utf-8',newline='\n')
artifact=o/'self.huab';artifact.unlink(missing_ok=True)
component=compiler/('_timed_archive.hua' if diagnostic else entry.name);bootstrap_report=None
compiler_hashes={path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(compiler.glob('*.hua'))}
if not diagnostic:assert compiler_hashes==hashes,'budget acceptance must execute unmodified compiler sources'
if bootstrap:
 target=o/'bootstrap-component.huab';t=time.monotonic()
 boot=subprocess.run([str(hua),'build',str(component),'-o',str(target)],capture_output=True,encoding='utf-8',timeout=600)
 assert boot.returncode==0,(boot.stdout,boot.stderr)
 bootstrap_report={'seconds':round(time.monotonic()-t,3),'bytes':target.stat().st_size,'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'producer':'C++ C0 bootstraps only the executing compiler; original own-source input never goes through C++ compilation'}
 component=target
start=time.monotonic();p=subprocess.Popen([str(hua),'run',str(component),'--execution-profile','compiler','--report-budget','--',str(source),str(artifact)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
class Counters(ctypes.Structure):
 _fields_=[('cb',ctypes.c_ulong),('faults',ctypes.c_ulong),('peak',ctypes.c_size_t),('working',ctypes.c_size_t),('quotaPeakPaged',ctypes.c_size_t),('quotaPaged',ctypes.c_size_t),('quotaPeakNonPaged',ctypes.c_size_t),('quotaNonPaged',ctypes.c_size_t),('pagefile',ctypes.c_size_t),('peakPagefile',ctypes.c_size_t)]
def cpu_seconds():
 if os.name!='nt':return None
 values=[ctypes.c_ulonglong() for _ in range(4)]
 if not ctypes.windll.kernel32.GetProcessTimes(ctypes.c_void_p(int(p._handle)),*(ctypes.byref(v) for v in values)):return None
 return (values[2].value+values[3].value)/10000000
peak=0 if os.name=='nt' else None;next_update=60;timeout=False;last_cpu=0.0 if os.name=='nt' else None;observations=[]
print(json.dumps({'stage':'self source captured','modules':len(seen),'input_bytes':sum((snapshot/name).stat().st_size for name in seen)}),flush=True)
while True:
 if os.name=='nt':
  c=Counters();c.cb=ctypes.sizeof(c)
  if ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(int(p._handle)),ctypes.byref(c),c.cb):peak=max(peak,c.peak)
 measured_cpu=cpu_seconds()
 if measured_cpu is not None:last_cpu=measured_cpu
 elapsed=time.monotonic()-start
 if elapsed>=next_update:
  record={'elapsed_seconds':round(elapsed),'cpu_seconds':round(last_cpu,3) if last_cpu is not None else None,'peak_working_mib':round(peak/1048576,1) if peak is not None else None}
  observations.append(record);(o/'observations.json').write_text(json.dumps(observations,indent=2)+'\n',encoding='utf-8');print(json.dumps(record),flush=True);next_update=max(next_update+60,(int(elapsed)//60+1)*60)
 remaining=b['supervisor_seconds']-elapsed
 if remaining<=0:p.kill();stdout,stderr=p.communicate();timeout=True;break
 try:stdout,stderr=p.communicate(timeout=min(2,remaining));break
 except subprocess.TimeoutExpired:pass
result={'returncode':p.returncode,'supervisor_timeout':timeout,'seconds':round(time.monotonic()-start,3),'peak_working_bytes':peak,'cpu_seconds':round(last_cpu,3) if last_cpu is not None else None,'report_bytes':len(stdout),'modules':len(seen),'input_bytes':sum((snapshot/name).stat().st_size for name in seen),'source_hashes':hashes,'emitter':'Hua instructions and complete HUAB encoding; existing native reader only','execution':'VM compiler profile; '+('C0 bootstrap persisted Hua compiler; ' if bootstrap else 'source Hua compiler; ')+'isolated full own-source pipeline; '+sys.platform,'bootstrap_component':bootstrap_report,'observations':observations,'runtime_sha256':hashlib.sha256(hua.read_bytes()).hexdigest(),'temporary_idle_sleep_prevention':awake,'diagnostic_phases':diagnostic,'diagnostic_helpers':helpers,'compiler_sources_identical':compiler_hashes==hashes,'compiler_source_hashes':compiler_hashes}
measured_cpu=cpu_seconds()
if measured_cpu is not None:result['cpu_seconds']=round(measured_cpu,3)
(o/'self-receipt.json').write_bytes(stdout);(o/'self-budget.json').write_bytes(stderr)
phases={line.split()[0]:int(line.split()[1]) for line in phasefile.read_text().splitlines()};base=phases.get('start',0);result['phases_ns']={k:v-base for k,v in phases.items()}
if p.returncode==0:
 receipt=json.loads(stdout);result['receipt']=receipt;result['status']=receipt['status'];result['metrics']=json.loads(stderr)
(o/'self-scale-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result),flush=True)
assert p.returncode==0 and not timeout and result.get('status')=='ok' and result['seconds']<600
assert result['metrics']['steps']<=500000000 and result['metrics']['peak_calls']<=128
assert len(stdout)<=b['report_bytes'] and artifact.stat().st_size==receipt['file_bytes']
calls=[]
def call(args):
 start=time.monotonic();v=subprocess.run(list(map(str,args)),capture_output=True,encoding='utf-8',timeout=600)
 assert v.returncode==0,(args,v.returncode,v.stdout[:2000],v.stderr[:3000]);calls.append({'action':str(args[1]),'seconds':round(time.monotonic()-start,3),'returncode':v.returncode});return v
call([hua,'check',artifact])
for name,digest in hashes.items():assert hashlib.sha256((repo/'selfhost'/name).read_bytes()).hexdigest()==digest
assert snapshot.parent==o and snapshot.name=='current-source'
for file in snapshot.glob('*.hua'):file.unlink()
assert not list(snapshot.glob('*.hua'))
sample=repo/'examples/frontend_modules/main.hua';reference=o/'reference.huab';generated=o/'own-generated.huab'
a=call([hua,'run',entry,'--execution-profile','compiler','--',sample,reference]);v=call([hua,'run',artifact,'--execution-profile','compiler','--',sample,generated])
assert json.loads(a.stdout)==json.loads(v.stdout) and reference.read_bytes()==generated.read_bytes()
call([hua,'check',generated]);a=call([hua,'run',reference]);v=call([hua,'run',generated]);assert a.stdout==v.stdout=='2 2\n'
result['own_compiler_execution']={'source_removed':True,'huab_bytes':artifact.stat().st_size,'huab_sha256':hashlib.sha256(artifact.read_bytes()).hexdigest(),'sample_bytes_identical':True,'sample_output':v.stdout,'calls':calls,'compiler':'Hua source -> Hua instructions -> Hua HUAB; executed by existing C++ reader/VM','generations':'C08 same-source compiler1->2->3 still pending'}
(o/'self-scale-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('complete current C07 own-source encoding and source-free compiler execution passed')
