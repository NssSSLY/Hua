"""BO-11 per-body capability work. Diagnostic only: never formal own-source, generations or budget changes."""
from pathlib import Path
import argparse,json,statistics,sys
from audit_bootstrap_phases import command,digest,save,closure,reduce_events
ROOT=Path(__file__).resolve().parents[1]
READMIT=ROOT/'build/bo11-readmission-20261018'
CASES={'type_rules':ROOT/'selfhost/type_rules.hua','ast_pool':ROOT/'selfhost/ast_pool.hua','parser':ROOT/'selfhost/parser_extended_main.hua',
 'modules':ROOT/'selfhost/modules.hua','holdout':READMIT/'holdout-source/bo11_holdout.hua','capabilities':ROOT/'selfhost/capabilities.hua'}
ORDINARY={'type_rules','ast_pool','parser','modules','holdout'}
SAMPLE_LIMIT=499999999
def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def prepare(out):
 runtime=ROOT/'build/hua.exe';c0=out/'compiler.huab'
 sources={n:digest(p) for n,p in closure(ROOT/'selfhost/archive_main.hua').items()}
 cases={n:{'entry':str(e),'sources':{str(p):digest(p) for p in closure(e).values()}} for n,e in CASES.items()}
 census={}
 for n,c in cases.items():
  _,stdout,_=command(ROOT,out,'body-census-'+n,[out/'body-census.exe',*sorted(c['sources'])],60);census[n]=json.loads(stdout)['functions']
 save(out/'census.json',census)
 save(out/'manifest.json',{'schema':'hua.bootstrap.body-work.v1','runtime':digest(runtime),'compiler':digest(c0),'compiler_sources':sources,'cases':cases,
  'tools':{p:digest(ROOT/p) for p in ['tests/bootstrap_capability_profile.hpp','tests/bootstrap_capability_sample.cpp','tests/bootstrap_body_census.cpp','tools/audit_bootstrap_body_work.py']},
  'sample_limit':SAMPLE_LIMIT,'production_limits':[500000000,128,65536,600,16777216],'full_own_source_runs':0,'generations':0})
def measure(out,name):
 m=load(out/'manifest.json');assert digest(ROOT/'build/hua.exe')==m['runtime'] and digest(out/'compiler.huab')==m['compiler']
 for p,sha in m['cases'][name]['sources'].items():assert digest(p)==sha,p
 d=out/'observations'/name;d.mkdir(parents=True,exist_ok=True);entry=m['cases'][name]['entry'];row={}
 if name in ORDINARY:
  r,so,se=command(ROOT,out,'ordinary-'+name,[ROOT/'build/hua.exe','run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',entry,d/'ordinary.huab'],300)
  row['ordinary']={k:r[k] for k in ['seconds','cpu_seconds','peak_working_bytes','returncode']};row['ordinary']['budget']=json.loads(se.decode().strip().splitlines()[-1])
 r,so,se=command(ROOT,out,'sample-'+name,[out/'capability-sample.exe',d/'events.jsonl',out/'compiler.huab',entry,d/'sample.huab',SAMPLE_LIMIT],400)
 summary=json.loads(se.decode().strip().splitlines()[-1]);save(d/'profile.json',summary)
 row['sample']={'seconds':r['seconds'],'complete':summary['complete'],'steps':summary['steps'],'peak_calls':summary['peak_calls']}
 if name in ORDINARY:
  row['same_huab']=(d/'ordinary.huab').read_bytes()==(d/'sample.huab').read_bytes()
  row['same_steps']=row['ordinary']['budget']['steps']==summary['steps'] and row['ordinary']['budget']['peak_calls']==summary['peak_calls']
 events=[json.loads(l) for l in (d/'events.jsonl').read_text(encoding='utf-8').splitlines()]
 row['phases']=reduce_events(events) if summary['complete'] else None
 cap=[e for e in events if e['phase']=='capabilities']
 row['capability_steps']=(cap[-1]['counters']['vm_steps']-cap[0]['counters']['vm_steps']) if len(cap)>=2 else (summary['steps']-cap[0]['counters']['vm_steps'] if cap else 0)
 save(d/'row.json',row);print(json.dumps({name:row['sample'],'same':[row.get('same_huab'),row.get('same_steps')]}),flush=True)
def analyze(out):
 censuses=load(out/'census.json');result={}
 for name in CASES:
  d=out/'observations'/name
  if not (d/'row.json').exists():continue
  row=load(d/'row.json');p=load(d/'profile.json');bodies=[];census=censuses[name]
  for body,visits in p['body_visit_detail'].items():
   c=census.get(body);steps=[v['steps'] for v in visits]
   vol={};hx={}
   for v in visits:
    for k,x in v['volume'].items():vol[k]=vol.get(k,0)+x
    for k,x in v['helper_exclusive'].items():hx[k]=hx.get(k,0)+x
   bodies.append({'body':body,'nodes':c['nodes'] if c else None,'visits':len(steps),'steps':sum(steps),'per_visit':steps,
    'steps_per_visit_node':(sum(steps)/len(steps)/c['nodes']) if c and c['nodes'] else None,'volume':vol,'top_helpers':sorted(hx.items(),key=lambda x:-x[1])[:6]})
  bodies.sort(key=lambda b:-b['steps']);total=sum(b['steps'] for b in bodies)
  unmatched=[b['body'] for b in bodies if b['nodes'] is None and b['body']]
  weighted=sum(b['nodes']*b['visits'] for b in bodies if b['nodes'])
  rates=[b['steps_per_visit_node'] for b in bodies if b['steps_per_visit_node'] and b['nodes']>=50]
  result[name]={'complete':row['sample']['complete'],'total_steps':row['sample']['steps'],'capability_steps':row['capability_steps'],'body_steps':total,
   'body_share_of_capability':total/row['capability_steps'] if row['capability_steps'] else None,'bodies':len(bodies),'visits':sum(b['visits'] for b in bodies),
   'weighted_nodes':weighted,'steps_per_weighted_node':total/weighted if weighted else None,'median_rate_nodes_ge_50':statistics.median(rates) if rates else None,
   'top10_share':sum(b['steps'] for b in bodies[:10])/total if total else None,'unmatched':unmatched,'volumes':{k:v for k,v in p['volumes'].items() if not k.startswith('shape-max')},
   'top':bodies[:15],'same_huab':row.get('same_huab'),'same_steps':row.get('same_steps'),'ordinary':row.get('ordinary')}
 save(out/'body-work-analysis.json',result);return result
def main():
 a=argparse.ArgumentParser();a.add_argument('out',type=Path);a.add_argument('step',choices=['prepare','measure','analyze']);a.add_argument('cases',nargs='*');x=a.parse_args()
 out=x.out.resolve()
 if x.step=='prepare':prepare(out)
 elif x.step=='measure':
  for n in x.cases or list(CASES):measure(out,n)
 else:json.dump({n:{k:v for k,v in r.items() if k!='top'} for n,r in analyze(out).items()},sys.stdout,indent=1,ensure_ascii=False)
if __name__=='__main__':main()
