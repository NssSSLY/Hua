"""Compare frozen C04 components on reproducible capability-analysis workloads.

This is a manual diagnostic, not a replacement for four-engine/oracle gates.
The optional unbounded test mode retains the 600-second compiler invocation cap.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess, time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--hua',type=Path,required=True)
p.add_argument('--before',type=Path)
p.add_argument('--after',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--case',choices=('fields','branches','updates','dependencies','projections','calls','effects','alias_effects','facts','all'),default='all')
p.add_argument('--unbounded',action='store_true',help='Use the production 600s limit instead of the default 30s diagnostic stop')
p.add_argument('--allow-work-count-change',action='store_true',help='Compare every report field except stats.work when dependency scheduling changes visited work')
a=p.parse_args()
exe=a.hua.resolve();after=a.after.resolve();before=a.before.resolve() if a.before else None
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
if (out/'results.json').exists():p.error('output already contains evidence; choose a new directory')
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
for path in (exe,after,*([before] if before else [])):
 if not path.is_file():p.error('missing executable/component: '+str(path))
rows=[];limit=600 if a.unbounded else 30
result=dict(schema='hua.internal.capability.performance.v1',status='running',compiler_limit_seconds=600,test_limit_seconds=limit,runtime_sha256=digest(exe),component_sha256={'after':digest(after),**({'before':digest(before)} if before else {})},rows=rows)
def save():
 (out/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')

def workload(case):
 if case=='facts':
  text='struct State{next []State\n'+''.join('v'+str(i)+' int\n' for i in range(32))+'}\nfn keep(s State)State{return s}\nfn visit(s State){\n'
  text+='keep(s)\n'*64+'}\n'
 elif case=='alias_effects':
  text='struct Cell{'+''.join('v'+str(i)+' int\n' for i in range(64))+'}\nfn change(cells mut []Cell){\n'
  text+=''.join('cells[0].v'+str(i)+'='+str(i)+'\n' for i in range(64))+'}\nfn caller(cells mut []Cell){var alias=cells\n'+'change(alias)\n'*10+'}\n'
 elif case=='effects':
  text='struct State{next []State\n'+''.join('v'+str(i)+' int\n' for i in range(64))+'}\nfn change(s mut State){\n'
  text+=''.join('s.v'+str(i)+'='+str(i)+'\n' for i in range(64))+'}\nfn caller(s mut State){\n'+'change(s)\n'*10+'}\n'
 elif case=='calls':
  text='struct State{tag int\nnext []State\n'+''.join('v'+str(i)+' int\n' for i in range(64))+'}\nfn get(s State)int{return s.tag}\nfn read(s State)int{var n=0\n'
  text+='n+=get(s)\n'*128+'return n}\n'
 elif case=='projections':
  text='struct State{tag int\nnext []State\n'+''.join('v'+str(i)+' int\n' for i in range(64))+'}\nfn read(s State)int{var n=0\n'
  text+='n+=s.tag\n'*128+'return n}\n'
 elif case=='dependencies':
  text=''
  for i in range(40):text+='fn a'+str(i).zfill(2)+'() int{return a'+str(i+1).zfill(2)+'()}\n'
  text+='fn a40() int{return 1}\n'
  for i in range(128):text+='fn unrelated'+str(i)+'(x int)int{return x}\n'
 elif case=='fields':
  text='struct Link{next []Link\ntag int}\n'
  for i in range(128):text+='struct Unused'+str(i)+'{'+''.join('x'+str(j)+' int\n' for j in range(8))+'}\n'
  for i in range(64):text+='fn f'+str(i)+'(x mut Link) Link{return x}\n'
 else:
  text='struct State{a []State\nb []State\nc []State\nd []State\ntag int}\n'
  if case=='updates':
   text+='fn choose(s mut State,n int)State{var c=s\n'
   for i in range(12):text+='if n=='+str(i)+'{c.tag='+str(i)+'}else{c.tag='+str(i+1)+'}\n'
   text+='return c}\n'
  else:
   text+='fn choose(s mut State,n int)int{var x=0\n'
   for i in range(12):text+='if n=='+str(i)+'{x+=s.tag}\n'
   text+='return x}\n'
 return text+'fn main(){}\n'

try:
 for case in (('fields','branches','updates','dependencies','projections','calls','effects','alias_effects','facts') if a.case=='all' else (a.case,)):
  source=out/(case+'.hua');source.write_text(workload(case),encoding='utf-8',newline='\n');reports=[]
  for version,component in ([('before',before)] if before else [])+[('after',after)]:
   command=[str(exe),'run',str(component),'--execution-profile','compiler','--report-budget','--',str(source)]
   start=time.monotonic();row=dict(case=case,version=version,command=command,input_sha256=digest(source));rows.append(row)
   try:r=subprocess.run(command,capture_output=True,timeout=limit)
   except subprocess.TimeoutExpired as error:
    row.update(status='timeout',seconds=time.monotonic()-start)
    (out/(case+'-'+version+'.stdout')).write_bytes(error.stdout or b'')
    (out/(case+'-'+version+'.stderr')).write_bytes(error.stderr or b'')
    raise
   row.update(seconds=time.monotonic()-start,code=r.returncode,report_sha256=hashlib.sha256(r.stdout).hexdigest())
   (out/(case+'-'+version+'.stdout')).write_bytes(r.stdout);(out/(case+'-'+version+'.stderr')).write_bytes(r.stderr)
   assert r.returncode==0,(command,r.returncode,r.stderr[-2000:])
   assert len(r.stdout)<=16777216,'report budget exceeded'
   report=json.loads(r.stdout);budget=json.loads(r.stderr);row['budget']=budget
   assert report.get('status')=='ok',report
   assert (budget['per_executor_step_limit'],budget['call_depth_limit'],budget['collection_interval'])==(500000000,128,65536)
   assert budget['steps']<=500000000 and budget['peak_calls']<=128
   row['status']='passed';reports.append(r.stdout);save();print(json.dumps(row,ensure_ascii=False),flush=True)
  if before:
   if a.allow_work_count_change:
    left,right=map(json.loads,reports);work=[]
    for report in (left,right):work.append(report.get('stats',{}).pop('work',None))
    assert left==right,case+': reports differ beyond stats.work'
    result.setdefault('work_counts',{})[case]={'before':work[0],'after':work[1]}
   else:assert reports[0]==reports[1],case+': complete public reports differ'
   result['comparison']='all report fields except stats.work' if a.allow_work_count_change else 'complete public report bytes'
   result.setdefault('equivalent_cases',[]).append(case)
 result['status']='passed';save()
except BaseException as error:
 result.update(status='failed',error=str(error));save();raise
