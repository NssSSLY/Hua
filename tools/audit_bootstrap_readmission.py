"""BO-11 new-baseline judgment. Never launches formal own-source or generations."""
from pathlib import Path
import argparse,itertools,json,math,shutil,sys,time
from audit_bootstrap_phases import command,digest,save,closure,reduce_events
ROOT=Path(__file__).resolve().parents[1]
def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def guard(out):
 m=load(out/'manifest.json')
 if (out/'prepared.json').exists():
  prepared=load(out/'prepared.json');assert digest(out/'features.json')==prepared['features_sha256'];assert digest(out/'manifest.json')==prepared['protocol_sha256']
 for p,sha in m['identities'].items():assert digest(ROOT/p)==sha,p
 assert {n:digest(p) for n,p in closure(ROOT/'selfhost/archive_main.hua').items()}==m['compiler_sources']
 for c in m['cases'].values():
  for p,sha in c['sources'].items():assert digest(Path(p))==sha,p
 return m

def prepare(out):
 m=guard(out);cc=ROOT/'.tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe'
 common=[cc,'-std=c++20','-O3','-DNDEBUG','-municode','-static','-I'+str(ROOT/'include'),'-I'+str(ROOT/'tests')]
 libraries=[ROOT/'build/libhua_semantics.a',ROOT/'build/libhua_frontend.a','-lpthread',ROOT/'.tools/wasmtime-v49.0.2-x86_64-windows-c-api/lib/wasmtime.dll.lib']
 command(ROOT,out,'build-phase-observer',common+[ROOT/'tests/bootstrap_phase_probe.cpp',out/'vm-observed.cpp',*libraries,'-o',out/'phase-observer.exe'],120)
 command(ROOT,out,'build-work-census',common+[ROOT/'tests/bootstrap_work_probe.cpp',*libraries,'-o',out/'work-census.exe'],90)
 artifacts={'parser':ROOT/'build/bo-step3-value-20261018/parser-after.huab','modules':ROOT/'build/bo-step3-value-20261018/modules-after.huab','full':out/'compiler.huab'}
 for name in ['type_rules','ast_pool','holdout']:
  artifact=out/(name+'-native.huab');command(ROOT,out,'native-proxy-'+name,[ROOT/'build/hua.exe','build',m['cases'][name]['entry'],'-o',artifact],90);artifacts[name]=artifact
 features={}
 for name,artifact in artifacts.items():
  paths=closure(ROOT/'selfhost/archive_main.hua') if name=='full' else closure(Path(m['cases'][name]['entry']))
  _,stdout,_=command(ROOT,out,'census-'+name,[out/'work-census.exe',artifact,*paths.values()],30);features[name]=json.loads(stdout)
 save(out/'features.json',{'rows':features,'artifacts':{n:{'path':str(p),'sha256':digest(p)} for n,p in artifacts.items()},'native_full_is_structural_proxy_only':True})
 tool_files=[ROOT/'tests/bootstrap_phase_profile.hpp',ROOT/'tests/bootstrap_phase_probe.cpp',ROOT/'tests/bootstrap_work_probe.cpp',ROOT/'tools/audit_bootstrap_readmission.py',out/'vm-observed.cpp',out/'phase-observer.exe',out/'work-census.exe']
 save(out/'prepared.json',{'status':'prepared before training or holdout execution','identities':{str(p):digest(p) for p in tool_files},'features_sha256':digest(out/'features.json'),'protocol_sha256':digest(out/'manifest.json')})
 print('Prepared ordinary observer and all pre-execution structural features',flush=True)

def measure(out,name):
 m=guard(out);prepared=load(out/'prepared.json')
 for p,sha in prepared['identities'].items():assert digest(Path(p))==sha,p
 dest=out/'observations'/name;dest.mkdir(parents=True,exist_ok=False)
 entry=Path(m['cases'][name]['entry']);normal=dest/'ordinary.huab';observed=dest/'observed.huab'
 ordinary,stdout,stderr=command(ROOT,out,'ordinary-'+name,[ROOT/'build/hua.exe','run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',entry,normal],200)
 receipt=json.loads(stdout);budget=json.loads(stderr);assert receipt['status']=='ok' and budget['steps']<=500000000 and budget['peak_calls']<=128
 row,ps,pe=command(ROOT,out,'phase-'+name,[out/'phase-observer.exe',dest/'events.jsonl',out/'compiler.huab',entry,observed],200)
 report=json.loads(pe);assert report['complete'] and report['steps']==budget['steps'] and report['peak_calls']==budget['peak_calls']
 assert stdout==ps and digest(normal)==digest(observed)
 events=[json.loads(s) for s in (dest/'events.jsonl').read_text(encoding='utf-8').splitlines()];phase=reduce_events(events)
 assert sum(r['counters']['vm_steps'] for r in phase.values())==budget['steps']
 _,fs,_=command(ROOT,out,'output-census-'+name,[out/'work-census.exe',normal,*closure(entry).values()],30)
 actual=json.loads(fs);assert actual==load(out/'features.json')['rows'][name],name
 command(ROOT,out,'reader-'+name,[ROOT/'build/hua.exe','check',normal],30)
 result={'status':'passed','ordinary':ordinary,'observer':row,'receipt':receipt,'budget':budget,'profile':report,'phases':{n:{'steps':r['counters']['vm_steps'],'seconds':r['ns']/1e9,'cpu_seconds':r['cpu_ns']/1e9,'entries':r['entries']} for n,r in phase.items()},'artifact_sha256':digest(normal),'exact_step_partition':True,'same_all_stdout_and_artifact_bytes':True,'observer_cpu_ratio':row['cpu_seconds']/ordinary['cpu_seconds'],'observer_wall_ratio':row['seconds']/ordinary['seconds']}
 save(dest/'result.json',result)
 print(json.dumps({'case':name,'steps':budget['steps'],'ordinary':ordinary['seconds'],'observed':row['seconds'],'cpu_ratio':result['observer_cpu_ratio'],'body_visits':sum(v for n,v in report['body_visits'].items() if n),'distinct_bodies':sum(bool(n) for n in report['body_visits'])}),flush=True)
 return result

def solve(matrix,values):
 a=[list(map(float,row))+[float(y)] for row,y in zip(matrix,values)];n=len(values)
 for i in range(n):
  pivot=max(range(i,n),key=lambda j:abs(a[j][i]))
  if abs(a[pivot][i])<1e-12:return None
  a[i],a[pivot]=a[pivot],a[i];d=a[i][i];a[i]=[x/d for x in a[i]]
  for j in range(n):
   if j!=i:
    d=a[j][i];a[j]=[x-d*y for x,y in zip(a[j],a[i])]
 return [row[-1] for row in a]

def fit(rows,targets,features):
 assert len(rows)>=2 and len(rows)==len(targets) and features
 assert all(math.isfinite(y) and y>=0 for y in targets)
 scale={f:max(float(r[f]) for r in rows) for f in features};usable=[f for f in features if scale[f]>0]
 assert all(math.isfinite(float(r[f])) and r[f]>=0 for r in rows for f in features)
 if max(targets)==0:return {'coefficients':{usable[0]:0.0} if usable else {},'score':0.0,'predicted_training':[0.0]*len(targets),'actual_training':targets,'max_observed_over_predicted':1.0}
 best=None
 for size in range(1,len(usable)+1):
  for names in itertools.combinations(usable,size):
   x=[[r[f]/scale[f] for f in names] for r in rows]
   # Relative residuals prevent the largest training input from hiding small-input failure.
   weights=[(max(targets)/max(y,max(targets)*1e-9))**2 for y in targets]
   matrix=[[sum(w*r[i]*r[j] for r,w in zip(x,weights)) for j in range(size)] for i in range(size)]
   values=[sum(w*r[i]*y for r,w,y in zip(x,weights,targets)) for i in range(size)]
   coeff=solve(matrix,values)
   if coeff is None or any(not math.isfinite(v) or v<0 for v in coeff):continue
   pred=[sum(c*t for c,t in zip(coeff,r)) for r in x];error=sum(((p-y)/max(y,1e-9))**2 for p,y in zip(pred,targets))
   if best is None or error<best['score']-1e-10:best={'coefficients':{f:c/scale[f] for f,c in zip(names,coeff)},'score':error,'predicted_training':pred,'actual_training':targets}
 assert best is not None,'no nonnegative model'
 best['max_observed_over_predicted']=max([1]+[y/p for p,y in zip(best['predicted_training'],targets) if p>0])
 return best

def predict(model,row):
 assert all(math.isfinite(float(row[f])) and row[f]>=0 and math.isfinite(c) and c>=0 for f,c in model['coefficients'].items())
 return sum(c*row[f] for f,c in model['coefficients'].items())

def freeze_prediction(out):
 m=guard(out);assert not (out/'prediction-before-holdout.json').exists() and not (out/'observations/holdout').exists()
 features=load(out/'features.json')['rows'];train=m['protocol']['training'];observed={n:load(out/'observations'/n/'result.json') for n in train}
 phase_names=sorted(observed[train[-1]]['phases']);assert all(set(r['phases'])==set(phase_names) for r in observed.values())
 major=[p for p in phase_names if any(max(r['phases'][p]['steps']/r['budget']['steps'],r['phases'][p]['seconds']/sum(x['seconds'] for x in r['phases'].values()))>=.05 for r in observed.values())]
 model={};predictions={}
 for metric in ['steps','seconds']:
  model[metric]={p:fit([features[n] for n in train],[observed[n]['phases'][p][metric] for n in train],m['protocol']['feature_map'][p]) for p in phase_names}
  predictions[metric]={n:{p:predict(mod,features[n]) for p,mod in model[metric].items()} for n in ['holdout','full']}
 calibrated=all(abs(observed[n]['observer_cpu_ratio']-1)<=.10 and abs(observed[n]['observer_wall_ratio']-1)<=.10 for n in ['parser','modules'])
 data={'status':'numerically frozen before holdout execution','created_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'manifest_sha256':digest(out/'manifest.json'),'features_sha256':digest(out/'features.json'),'training':train,'major_phases':major,'model':model,'predictions':predictions,'ordinary_timing_calibrated':calibrated,'training_results_sha256':{n:digest(out/'observations'/n/'result.json') for n in train},'holdout_entry_sha256':digest(Path(m['cases']['holdout']['entry'])),'first_failure_stops':True,'full_forecast_is_unvalidated_until_holdout':True}
 save(out/'prediction-before-holdout.json',data)
 print(json.dumps({'status':data['status'],'major_phases':major,'holdout_steps':sum(predictions['steps']['holdout'].values()),'full_steps_unvalidated':sum(predictions['steps']['full'].values()),'ordinary_timing_calibrated':calibrated}),flush=True)

def holdout(out):
 p=load(out/'prediction-before-holdout.json');assert p['status']=='numerically frozen before holdout execution'
 assert sum(p['predictions']['steps']['holdout'].values())*1.25<=450000000,'holdout preflight exceeds supervision work margin; no VM run'
 sha=digest(out/'prediction-before-holdout.json');r=measure(out,'holdout');assert digest(out/'prediction-before-holdout.json')==sha
 errors={metric:{n:100*(p['predictions'][metric]['holdout'][n]/v[metric]-1) for n,v in r['phases'].items()} for metric in ['steps','seconds']}
 save(out/'holdout-evaluation.json',{'prediction_sha256':sha,'errors_percent':errors,'major_phases':p['major_phases'],'major_passed':all(abs(errors[k][n])<=25 for k in errors for n in p['major_phases']),'observed_holdout_steps':r['budget']['steps'],'observed_holdout_ordinary_seconds':r['ordinary']['seconds'],'all_phase_errors_shown':True})
 print(json.dumps(load(out/'holdout-evaluation.json')),flush=True)

def decide(out):
 from audit_bootstrap_scale import decide as gates
 m=guard(out);p=load(out/'prediction-before-holdout.json');evaluation=load(out/'holdout-evaluation.json');observed=load(out/'observations/holdout/result.json')
 assert evaluation['prediction_sha256']==digest(out/'prediction-before-holdout.json') and p['features_sha256']==digest(out/'features.json')
 train={n:load(out/'observations'/n/'result.json') for n in p['training']}
 assert all(digest(out/'observations'/n/'result.json')==sha for n,sha in p['training_results_sha256'].items())
 major=p['major_phases'];ratios={}
 for metric in ['steps','seconds']:
  ratios[metric]={n:max(mod['max_observed_over_predicted'],observed['phases'][n][metric]/max(p['predictions'][metric]['holdout'][n],1e-9),1) for n,mod in p['model'][metric].items()}
 central_steps=sum(p['predictions']['steps']['full'].values());candidate_steps=1.25*sum(value*ratios['steps'][n] for n,value in p['predictions']['steps']['full'].items())
 ordinary_scale=max([1]+[r['ordinary']['seconds']/sum(x['seconds'] for x in r['phases'].values()) for r in [*train.values(),observed]])
 central_seconds=ordinary_scale*sum(p['predictions']['seconds']['full'].values());guarded_seconds=1+1.25*ordinary_scale*sum(value*ratios['seconds'][n] for n,value in p['predictions']['seconds']['full'].items())
 timing=p['ordinary_timing_calibrated'] and abs(observed['observer_cpu_ratio']-1)<=.10 and abs(observed['observer_wall_ratio']-1)<=.10
 # Fixed C07 success receipt; binary image size is not the report size.
 max_int=2**63-1;receipt={'schema':'hua.archive.result.v1','profile':'C07-HUAB1','status':'ok','source':'archive_main.hua','format':max_int,'file_bytes':max_int,'payload_bytes':max_int,'crc32':max_int,'instructions':max_int}
 receipt_bytes=len(json.dumps(receipt,separators=(',',':')).encode())+1;assert receipt_bytes<1024
 gate=gates(step_errors={n:evaluation['errors_percent']['steps'][n] for n in major},wall_errors={n:evaluation['errors_percent']['seconds'][n] for n in major},guarded_seconds=guarded_seconds,projected_steps=candidate_steps,correctness=True,structural_verified=True,ordinary_timing_verified=timing,call_depth_verified=False,report_limit_verified=True)
 training_errors={metric:{phase:[100*(a-b)/max(b,1e-9) for a,b in zip(mod['predicted_training'],mod['actual_training'])] for phase,mod in values.items()} for metric,values in p['model'].items()}
 if any(abs(e)>25 for metric in training_errors for phase in major for e in training_errors[metric][phase]):gate['reasons'].append('major training phase residual exceeds25 percent');gate['admitted']=False;gate['formal_attempts_authorized_by_this_audit']=0
 result={'schema':'hua.bootstrap.readmission.v1','status':'judgment complete','decision':gate,'major_phases':major,'holdout_errors_percent':evaluation['errors_percent'],'training_errors_percent':training_errors,'reduced_holdout_major_passed':evaluation['major_passed'],'ordinary_timing_calibrated':timing,'ordinary_time_scale':ordinary_scale,'full_candidate':{'steps_central':central_steps,'steps_with_empirical_guard':candidate_steps,'ordinary_seconds_central':central_seconds,'ordinary_seconds_with_empirical_guard':guarded_seconds,'per_phase':p['predictions'],'warning':'Full forecasts are planning scenarios, not measured own-source results or proven bounds. If holdout fails they are invalid for admission.'},'forecast_valid_for_admission':evaluation['major_passed'] and timing,'report_success_bound_bytes':receipt_bytes,'report_limit_bytes':16777216,'call_depth':{'certified':False,'observed_peaks':{n:r['budget']['peak_calls'] for n,r in {**train,'holdout':observed}.items()},'reason':'Source AST depth and reduced observations do not prove full-input VM recursion <=128.'},'native_sha256':m['identities']['build/hua.exe'],'compiler_sha256':m['identities']['build/bo-step3-value-20261018/compiler.huab'],'compiler_sources':m['compiler_sources'],'features':load(out/'features.json')['rows'],'ordinary_cases':{n:{'seconds':r['ordinary']['seconds'],'cpu_seconds':r['ordinary']['cpu_seconds'],'peak_working_bytes':r['ordinary']['peak_working_bytes'],'steps':r['budget']['steps'],'calls':r['budget']['peak_calls']} for n,r in {**train,'holdout':observed}.items()},'semantic_body_visits':{n:r['profile']['body_visits'] for n,r in {**train,'holdout':observed}.items()},'production_limits':[500000000,128,65536,600,16777216],'full_own_source_runs':0,'generations':0,'correctness_reused':'build/bo-step3-value-20261018/summary.json; exact runtime/C0/source identities checked live','committed_or_pushed':False}
 save(out/'summary.json',result);print(json.dumps({'decision':gate,'major_phases':major,'holdout_passed':evaluation['major_passed'],'full_candidate_steps':candidate_steps,'full_candidate_seconds':guarded_seconds,'ordinary_timing_calibrated':timing}),flush=True)

def main():
 a=argparse.ArgumentParser(description=__doc__);a.add_argument('action',choices=['prepare','train','predict','holdout','decide']);a.add_argument('--out',type=Path,required=True);v=a.parse_args();out=v.out.resolve()
 if v.action=='prepare':prepare(out)
 elif v.action=='train':
  for name in guard(out)['protocol']['training']:measure(out,name)
 elif v.action=='predict':freeze_prediction(out)
 elif v.action=='holdout':holdout(out)
 else:decide(out)
if __name__=='__main__':main()
