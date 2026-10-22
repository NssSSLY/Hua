"""C07 width-aware admission; no full own-source execution is launched by this tool."""
from pathlib import Path
from functools import lru_cache
import argparse,json,sys,time,shutil,math
from audit_bootstrap_phases import command,digest,closure,save,reduce_events
from audit_bootstrap_readmission import fit,predict
ROOT=Path(__file__).resolve().parents[2]
TRAIN=['type_rules','modules','capabilities','bytecode']
FEATURES={'execution':['constant'],'types':['ast_nodes','source_bytes','parameter_shape'],'names':['ast_nodes','source_bytes','source_functions'],'parse':['tokens','source_bytes','constant'],'return-infer':['ast_nodes','source_functions','parameter_shape'],'capabilities':['ast_nodes','parameter_shape','state_exposure'],'codegen':['constant'],'lower':['ast_nodes','parameter_shape','source_functions'],'core-verify':['ast_nodes','instructions','constant'],'emit':['instructions','functions','constant'],'encode':['instructions','source_bytes','constant'],'bytecode-verify':['instructions','branches','functions'],'publish':['constant']}
def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def generic(t,name):
 if not t.startswith(name+'<') or not t.endswith('>'):return []
 out=[];start=len(name)+1;depth=0
 for i in range(start,len(t)-1):
  depth+=(t[i]=='<')-(t[i]=='>')
  if t[i]==',' and depth==0:out.append(t[start:i]);start=i+1
 return out+[t[start:-1]]
def width(g):
 @lru_cache(None)
 def count(t,level=0):
  if level>=8:return 0
  t=t.removesuffix('?');a=generic(t,'Result')
  if len(a)==2:t=a[0]
  if t in g['structures']:return sum(1+count(v,level+1) for v in g['structures'][t].values())
  child='';a=generic(t,'map');b=generic(t,'List')
  if len(a)==2:child=a[1]
  elif len(b)==1:child=b[0]
  elif t.startswith('[') and ']' in t:child=t[t.index(']')+1:]
  return 1+count(child,level+1) if child else 0
 return sum(count(t) for types in g['functions'].values() for t in types)
def guard(out):
 m=load(out/'manifest.json')
 for p,h in m['identities'].items():assert digest(Path(p))==h,p
 assert {n:digest(p) for n,p in closure(out/'source/archive_main.hua').items()}==m['sources']
 if (out/'prepared.json').exists():
  p=load(out/'prepared.json');assert digest(out/'features.json')==p['features'] and digest(out/'manifest.json')==p['manifest']
 return m
def prepare(out):
 assert not out.exists();out.mkdir();(out/'source').mkdir();parent=out.parent
 sources=closure(ROOT/'selfhost/archive_main.hua')
 for n,p in sources.items():shutil.copy2(p,out/'source'/n)
 shutil.copy2(parent/'after.huab',out/'compiler.huab')
 observer=ROOT/'tests/artifacts/build/bo11-readmission-20261018/phase-observer.exe';census=ROOT/'tests/artifacts/build/bo11-readmission-20261018/work-census.exe';shape=parent/'type-shape-probe-enum-excluded.exe'
 cases={n:out/'source'/(n+'.hua') for n in TRAIN};cases.update(holdout=out/'source/archive.hua',full=out/'source/archive_main.hua')
 identities=[ROOT/'tests/artifacts/build/hua.exe',out/'compiler.huab',observer,census,shape,Path(__file__).resolve(),ROOT/'tests/tools/audit_bootstrap_readmission.py']
 save(out/'manifest.json',{'created_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'identities':{str(p):digest(p) for p in identities},'sources':{n:digest(p) for n,p in sources.items()},'cases':{n:str(p) for n,p in cases.items()},'training':TRAIN,'holdout':'archive.hua: 13 real modules, not yet executed with current compiler','feature_map':FEATURES,'static_width_warning':'Native declaration proxy, excluding enum private fields; exact depth-8 shape counts cross-checked on three actual Modules shapes. Does not count body visits or dynamic origin volumes.','holdout_gate':{'major_phase_percent':5,'relative_error_percent':25,'ordinary_timing_perturbation_percent':10,'seconds':400,'production_limits':[500000000,128,65536,600,16777216]},'no_holdout_if_training_phase_error_exceeds_25':True})
 rows={}
 for n,entry in cases.items():
  artifact=out/(n+'-native.huab')
  if n=='full':artifact=out/'compiler.huab'
  else:command(ROOT,out,'native-features-'+n,[ROOT/'tests/artifacts/build/hua.exe','build',entry,'-o',artifact],90)
  _,raw,_=command(ROOT,out,'work-features-'+n,[census,artifact,*closure(entry).values()],20);f=json.loads(raw)
  _,raw,_=command(ROOT,out,'shape-features-'+n,[shape,artifact],20);g=json.loads(raw);f['parameter_shape']=width(g);rows[n]=f
 save(out/'features.json',rows);save(out/'prepared.json',{'manifest':digest(out/'manifest.json'),'features':digest(out/'features.json')})
 print('New width-aware protocol and features frozen before new training measurements',flush=True)
def measure(out,name):
 m=guard(out);assert name in TRAIN+['holdout'];dest=out/name;assert not dest.exists();dest.mkdir();entry=Path(m['cases'][name]);artifact=dest/'ordinary.huab'
 # Reuse the current repair's successful ordinary Modules control; no duplicate VM run.
 if name=='modules':
  prior=out.parent;ordinary=load(prior/'commands/ordinary-modules-after.json');stdout=(prior/'commands/ordinary-modules-after.stdout').read_bytes();stderr=(prior/'commands/ordinary-modules-after.stderr').read_bytes();shutil.copy2(prior/'modules-after.huab',artifact)
  assert digest(entry)==digest(ROOT/'selfhost/modules.hua');assert digest(out/'compiler.huab')==digest(prior/'after.huab')
 else:ordinary,stdout,stderr=command(ROOT,out,'ordinary-'+name,[ROOT/'tests/artifacts/build/hua.exe','run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',entry,artifact],250)
 receipt=json.loads(stdout);budget=json.loads(stderr);assert receipt['status']=='ok'
 observer=ROOT/'tests/artifacts/build/bo11-readmission-20261018/phase-observer.exe'
 row,ps,pe=command(ROOT,out,'phase-'+name,[observer,dest/'events.jsonl',out/'compiler.huab',entry,dest/'observed.huab'],250)
 report=json.loads(pe);assert report['complete'] and report['steps']==budget['steps'] and report['peak_calls']==budget['peak_calls'];assert ps==stdout and digest(artifact)==digest(dest/'observed.huab')
 phases=reduce_events([json.loads(x) for x in (dest/'events.jsonl').read_text(encoding='utf-8').splitlines()]);assert sum(v['counters']['vm_steps'] for v in phases.values())==budget['steps']
 command(ROOT,out,'reader-'+name,[ROOT/'tests/artifacts/build/hua.exe','check',artifact],20)
 result={'ordinary':ordinary,'observer':row,'receipt':receipt,'budget':budget,'phases':{p:{'steps':v['counters']['vm_steps'],'seconds':v['ns']/1e9} for p,v in phases.items()},'observer_cpu_ratio':row['cpu_seconds']/ordinary['cpu_seconds'],'observer_wall_ratio':row['seconds']/ordinary['seconds'],'profile':report,'ordinary_reused':name=='modules'}
 save(dest/'result.json',result);print(json.dumps({'case':name,'steps':budget['steps'],'wall':ordinary['seconds'],'observer_cpu_ratio':result['observer_cpu_ratio'],'capability_steps':result['phases']['capabilities']['steps']}),flush=True)
def freeze(out):
 m=guard(out);assert not (out/'prediction.json').exists() and not (out/'holdout').exists();features=load(out/'features.json');train={n:load(out/n/'result.json') for n in TRAIN};phases=list(FEATURES)
 major=[p for p in phases if any(max(r['phases'][p]['steps']/r['budget']['steps'],r['phases'][p]['seconds']/sum(v['seconds'] for v in r['phases'].values()))>=.05 for r in train.values())]
 models={metric:{p:fit([features[n] for n in TRAIN],[train[n]['phases'][p][metric] for n in TRAIN],FEATURES[p]) for p in phases} for metric in ['steps','seconds']}
 errors={metric:{p:[100*(a/b-1) if b else 0 for a,b in zip(mod['predicted_training'],mod['actual_training'])] for p,mod in pm.items()} for metric,pm in models.items()}
 passed=all(abs(e)<=25 for metric in errors.values() for p in major for e in metric[p])
 timing=all(abs(train[n]['observer_cpu_ratio']-1)<=.10 and abs(train[n]['observer_wall_ratio']-1)<=.10 for n in ['modules','capabilities','bytecode'])
 predictions={metric:{n:{p:predict(mod,features[n]) for p,mod in pm.items()} for n in ['holdout','full']} for metric,pm in models.items()}
 data={'status':'frozen before holdout','major':major,'models':models,'predictions':predictions,'training_errors_percent':errors,'training_passed':passed,'timing_calibrated':timing,'features_sha256':digest(out/'features.json'),'training_sha256':{n:digest(out/n/'result.json') for n in TRAIN},'no_holdout_if_training_failed':True}
 save(out/'prediction.json',data);print(json.dumps({'training_passed':passed,'timing_calibrated':timing,'holdout_steps':sum(predictions['steps']['holdout'].values()),'full_steps':sum(predictions['steps']['full'].values()),'major':major,'major_training_errors':{metric:{p:v for p,v in pm.items() if p in major} for metric,pm in errors.items()}}),flush=True)
def validate(out):
 p=load(out/'prediction.json');assert p['training_passed'] and p['timing_calibrated'],'No holdout run: fix training or timing cause first';assert sum(p['predictions']['steps']['holdout'].values())*1.25<500000000,'Holdout estimate lacks step margin'
 frozen=digest(out/'prediction.json');measure(out,'holdout');assert digest(out/'prediction.json')==frozen;r=load(out/'holdout/result.json');errors={metric:{phase:100*(pred/r['phases'][phase][metric]-1) if r['phases'][phase][metric] else 0 for phase,pred in rows['holdout'].items()} for metric,rows in p['predictions'].items()}
 passed=all(abs(e)<=25 for pm in errors.values() for phase,e in pm.items() if phase in p['major'])
 data={'phase_errors_percent':errors,'major_passed':passed,'prediction_sha256':frozen,'full_forecast_valid':passed and p['timing_calibrated'],'full_own_source_attempts':0,'warning':'Candidate forecasts require all admission gates; no full source execution performed.'};save(out/'evaluation.json',data);print(json.dumps(data),flush=True)
def main():
 a=argparse.ArgumentParser();a.add_argument('action',choices=['prepare','measure','freeze','validate']);a.add_argument('out',type=Path);a.add_argument('--case');v=a.parse_args();out=v.out.resolve()
 if v.action=='prepare':prepare(out)
 elif v.action=='measure':measure(out,v.case)
 elif v.action=='freeze':freeze(out)
 else:validate(out)
if __name__=='__main__':main()
