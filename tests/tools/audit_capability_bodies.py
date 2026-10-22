"""BO-11 follow-up: per-target visits and measured capability work; no admission run."""
from pathlib import Path
import argparse,collections,json,statistics,sys,unittest
from audit_bootstrap_phases import command,digest,save,closure,reduce_events
ROOT=Path(__file__).resolve().parents[2]
def load(path):return json.loads(Path(path).read_text(encoding='utf-8'))
def guard(out):
 m=load(out/'manifest.json');p=load(out/'prepared-runtime.json')
 for name,sha in m['production_files'].items():assert digest(ROOT/name)==sha,name
 assert digest(p['runtime'])==p['runtime_sha256'] and digest(out/'compiler.huab')==p['compiler_sha256']
 if (out/'prepared-observer.json').exists():
  for path,sha in load(out/'prepared-observer.json')['sources_and_binary'].items():assert digest(path)==sha,path
 return m,p
def prepare(out):
 m,p=guard(out);build=Path(p['build']);cc=ROOT/'.tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe'
 command(ROOT,out,'build-body-observer',[cc,'-std=c++20','-O3','-DNDEBUG','-municode','-static','-I'+str(ROOT/'include'),'-I'+str(ROOT/'tests'),
  ROOT/'tests/capability_body_probe.cpp',build/'libhua_semantics.a',build/'libhua_frontend.a','-lpthread',ROOT/'.tools/wasmtime-v49.0.2-x86_64-windows-c-api/lib/wasmtime.dll.lib','-o',out/'body-observer.exe'],90)
 command(ROOT,out,'reader-c0',[p['runtime'],'check',out/'compiler.huab'],30)
 _,stdout,_=command(ROOT,out,'native-body-census',[out/'body-observer.exe','--census',*closure(ROOT/'selfhost/archive_main.hua').values()],30)
 census=load_bytes(stdout);assert len(census['functions'])>200
 save(out/'body-census.json',census)
 paths=[ROOT/'tests/bootstrap_capability_profile.hpp',ROOT/'tests/capability_body_probe.cpp',ROOT/'tests/tools/audit_capability_bodies.py',out/'body-observer.exe']
 save(out/'prepared-observer.json',{'sources_and_binary':{str(p):digest(p) for p in paths},'census_sha256':digest(out/'body-census.json'),
  'cases':{n:{'entry':str(ROOT/'selfhost'/(n+'.hua')),'sources':{str(p):digest(p) for p in closure(ROOT/'selfhost'/(n+'.hua')).values()}} for n in m['cases']}})
def load_bytes(data):return json.loads(data.decode('utf-8'))
def validate_profile(report):
 assert report['complete'];visits=report['target_visits'];groups=collections.defaultdict(list);checks=0
 for visit in visits:
  assert sum(v['exclusive'] for v in visit['helpers'].values())==visit['steps'];checks+=1
  assert visit['steps']>0 and visit['ordinal']==len(groups[visit['target']])+1;checks+=1
  assert all(v>=0 for v in visit['volumes'].values());groups[visit['target']].append(visit)
 assert {k:len(v) for k,v in groups.items()}==report['body_visits'];checks+=1
 for name,values in groups.items():
  if name:assert sum(v['steps'] for v in values)==report['body_work'][name]['steps'];checks+=bool(name)
 work=report['helper_work'];assert sum(v['exclusive'] for v in work.values())==work['analyze']['inclusive'];checks+=1
 assert sum(v['steps'] for v in visits)<=work['analyze']['inclusive'];checks+=1
 return checks
def measure(out,name):
 m,p=guard(out);assert name in m['cases'];prepared=load(out/'prepared-observer.json');case=prepared['cases'][name]
 limit=m['protocol'].get('case_timeouts',{}).get(name,m['protocol']['case_timeout_seconds']);assert 1<=limit<=300
 for path,sha in case['sources'].items():assert digest(path)==sha,path
 dest=out/'cases'/name;dest.mkdir(parents=True,exist_ok=False);exe=out/'body-observer.exe';compiler=out/'compiler.huab'
 ordinary,stdout,stderr=command(ROOT,out,name+'-ordinary',[exe,dest/'ordinary-events.jsonl',compiler,case['entry'],dest/'ordinary.huab','off'],limit)
 off=load_bytes(stderr);assert off['complete'] and not off['target_visits']
 observed,obsout,obserr=command(ROOT,out,name+'-observed',[exe,dest/'events.jsonl',compiler,case['entry'],dest/'observed.huab'],limit)
 report=load_bytes(obserr);checks=validate_profile(report)
 assert stdout==obsout and digest(dest/'ordinary.huab')==digest(dest/'observed.huab')
 assert off['steps']==report['steps'] and off['peak_calls']==report['peak_calls'];checks+=2
 events=[json.loads(s) for s in (dest/'events.jsonl').read_text(encoding='utf-8').splitlines()];phases=reduce_events(events)
 assert sum(v['counters']['vm_steps'] for v in phases.values())==report['steps'];checks+=1
 assert phases['capabilities']['counters']['vm_steps']==report['helper_work']['analyze']['inclusive'];checks+=1
 command(ROOT,out,name+'-reader',[p['runtime'],'check',dest/'ordinary.huab'],30)
 save(dest/'result.json',{'status':'passed','case':name,'checks':checks,'ordinary':ordinary,'observed':observed,'same_receipt_and_artifact':True,
  'same_steps_and_calls':True,'profile':report,'phases':phases,'artifact_sha256':digest(dest/'ordinary.huab'),
  'observer_cpu_ratio':observed['cpu_seconds']/ordinary['cpu_seconds'],'ordinary_time_calibrated':False})
 print(json.dumps({'case':name,'checks':checks,'steps':report['steps'],'visits':len(report['target_visits']),'ordinary_seconds':ordinary['seconds'],'observed_seconds':observed['seconds']}),flush=True)
def analyze(out):
 m,p=guard(out);census=load(out/'body-census.json');index={(f['module'],f['name']):f for f in census['functions']};cases=[]
 assert len(index)==len(census['functions']),'ambiguous function identity'
 for name in m['cases']:
  data=load(out/'cases'/name/'result.json');assert data['status']=='passed';report=data['profile'];groups=collections.defaultdict(list)
  for v in report['target_visits']:
   if v['target']:groups[v['target']].append(v)
  targets=[]
  for target,visits in groups.items():
   ident=(Path(visits[0]['module']).name,target.split('#',1)[1]);assert ident in index,ident;nodes=index[ident]['body_nodes'];steps=[v['steps'] for v in visits]
   targets.append({'target':target,'module':ident[0],'function':ident[1],'body_nodes':nodes,'visits':len(visits),'steps':sum(steps),'per_visit_steps':steps,
    'steps_per_body_node':[v/nodes for v in steps],'revisited_steps':sum(steps[1:]),'max_over_min_visit':max(steps)/min(steps),
    'setup_values':[v['values_at_entry'] for v in visits],'probing':[v['probing'] for v in visits]})
  cap=report['helper_work']['analyze']['inclusive'];body=sum(t['steps'] for t in targets);global_work=sum(v['steps'] for v in report['target_visits'] if not v['target'])
  cases.append({'case':name,'checks':data['checks'],'ordinary_seconds':data['ordinary']['seconds'],'ordinary_cpu_seconds':data['ordinary']['cpu_seconds'],
   'observer_cpu_ratio':data['observer_cpu_ratio'],'capability_steps':cap,'target_body_steps':body,'global_body_steps':global_work,'setup_summary_other_steps':cap-body-global_work,
   'actual_passes':report['volumes']['actual-passes'],'logical_work':report['volumes']['logical-work'],'targets':sorted(targets,key=lambda t:t['steps'],reverse=True),
   'helpers_exclusive':sorted([{'helper':k,**v} for k,v in report['helper_work'].items()],key=lambda t:t['exclusive'],reverse=True),
   'shape_max_fields':{k[len('shape-max:'):]:v for k,v in report['volumes'].items() if k.startswith('shape-max:')},'volumes':report['volumes']})
 common=[]
 for key in sorted(set.union(*[{(t['module'],t['function']) for t in c['targets']} for c in cases])):
  rows={c['case']:next((t for t in c['targets'] if (t['module'],t['function'])==key),None) for c in cases}
  rows={name:row for name,row in rows.items() if row is not None}
  if len(rows)<2:continue
  assert len({row['body_nodes'] for row in rows.values()})==1,'same target body changed'
  common.append({'module':key[0],'function':key[1],'by_case':rows})
 commands=[load(p) for p in (out/'commands').glob('*.json')]
 failures=[v for v in commands if v['returncode'] or v['timeout']]
 assert all(v['label'] in ('configure','configure-forward-paths','build-runtime','prepare-native-c0') for v in failures),'an unreviewed workload failed'
 save(out/'summary.json',{'status':'bounded per-target evidence complete','cases':cases,'same_function_cross_case':common,'checks':sum(c['checks'] for c in cases),
  'command_count':len(commands),'timeouts':sum(c['timeout'] for c in commands),'retained_setup_failures':len(failures),'workload_timeouts':sum(c['timeout'] for c in commands if c['label'].startswith(tuple(m['cases']))),'full_own_source_vm_runs':0,'generations':0,
  'production_changed':False,'model_fit_or_admission':False,'limits':[500000000,128,65536,600,16777216],
  'boundary':'VM steps are inclusive from entry after binding through return/defer; helper exclusive deltas partition each target. Native GC/Value cost has no separate VM steps.',
  'volumes':'snapshot/alias/edited fields count backing metadata entries, not bytes, deep copies or visible prefix widths; shape-max comes from actual level-bounded field_defaults results.',
  'timing':'ordinary = same driver with observer disabled; observer cost and environment remain uncalibrated; no normal-stage time inference'})
 print(json.dumps({'status':'passed','cases':len(cases),'checks':sum(c['checks'] for c in cases),'timeouts':sum(c['timeout'] for c in commands)}),flush=True)
class Tests(unittest.TestCase):
 def test_missing_visit_rejected(self):
  with self.assertRaises(KeyError):validate_profile({'complete':True})
 def test_false_completion_rejected(self):
  with self.assertRaises(AssertionError):validate_profile({'complete':False})
 def test_unpartitioned_steps_rejected(self):
  with self.assertRaises(AssertionError):validate_profile({'complete':True,'target_visits':[{'steps':10,'helpers':{'a':{'exclusive':9}}}]})
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=['prepare','measure','analyze','self-test']);parser.add_argument('--out',type=Path);parser.add_argument('--case');args=parser.parse_args()
 if args.action=='self-test':return 0 if unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests)).wasSuccessful() else 1
 if args.out is None:parser.error('--out is required')
 out=args.out.resolve();assert out.is_relative_to(ROOT/'tests/artifacts') and out!=ROOT/'tests/artifacts'
 try:
  if args.action=='prepare':prepare(out)
  elif args.action=='measure':measure(out,args.case)
  else:analyze(out)
 except BaseException as e:
  failure=out/(args.action+('-'+args.case if args.case else '')+'-failure.json')
  if not failure.exists():save(failure,{'error':repr(e),'status':'stopped','automatic_retry':False})
  raise
 return 0
if __name__=='__main__':raise SystemExit(main())
