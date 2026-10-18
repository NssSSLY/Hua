"""BO-10 one-pass flat edge table: old collector oracle and bounded real evidence.
Uses explicitly frozen BO-09/10 records; no whole compiler own-source workload.
"""
from pathlib import Path
import argparse,json,re
from audit_bootstrap_phases import command,digest,save,closure
from audit_gc_cost import PHASES,VALUE_KINDS

def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def current(root,out):
    m=load(out/'manifest.json');assert digest(out/'compiler.huab')==m['compiler_sha256']
    assert {n:digest(p) for n,p in closure(root/'selfhost/archive_main.hua').items()}==m['compiler_sources']
    assert digest(root/'tests/reference/gc_nested_edges.cpp')==load(out/'reference-identity.json')['sha256']
    return m

# Representation-specific fields never get silently compared as equivalent units.
DIFFERENT={'schema','start_ns','total_ns','phases_ns','adjacency_growths','adjacency_outer_bytes','adjacency_capacity_bytes'}
def equivalent(before,after):
    assert before['schema']=='hua.gc.cost.v1' and after['schema']=='hua.gc.cost.v2'
    for key in before:
        if key not in DIFFERENT:assert before[key]==after[key],(key,before[key],after[key])
    assert list(after['phases_ns'])==PHASES and sum(after['phases_ns'].values())==after['total_ns']
    assert after['adjacency_growths']==after['adjacency_outer_bytes']==after['adjacency_capacity_bytes']==0
    assert after['offsets_capacity_bytes']==8*(after['live_before']+1)
    assert after['flat_edge_capacity_bytes']>=after['tracked_edges']*8
    assert after['flat_edge_growths']<=64
    assert (after['flat_edge_growths']==0)==(after['tracked_edges']==0)

def calibrate(root,out):
    current(root,out);old=load(root/'build/bo10-gc-20261017/calibration.json')['rows'];rows={};name=None
    for line in (out/'commands/flat-cost-fixtures-r2.stdout').read_text(encoding='utf-8').splitlines():
        row=json.loads(line)
        if 'fixture' in row:name=row['fixture']
        else:assert name not in rows;rows[name]=row
    assert rows.keys()==old.keys()
    for name,row in rows.items():equivalent(old[name],row)
    save(out/'cost-equivalence.json',{'status':'passed','fixtures':len(rows),'all_old_semantic_fields_equal':True,'old_nested_growths':sum(r['adjacency_growths'] for r in old.values()),'new_flat_growths':sum(r['flat_edge_growths'] for r in rows.values()),'rows':rows})
    print('17 diagnostic graph rows: all old semantic facts unchanged',flush=True)

def run_case(root,out,name):
    m=current(root,out);assert load(out/'graph-equivalence.json')['status']==load(out/'cost-equivalence.json')['status']=='passed'
    old=next(r for r in load(root/'build/bo09-vm-20261016/real-final/results.json')['rows'] if r['case']==name)
    diagnostic=load(root/'build/bo10-gc-20261017/cases'/name/'result.json')
    assert diagnostic['probe_sha256']==m['tools']['probe']['sha256']
    assert load(root/'build/bo09-vm-20261016/summary.json')['native_sha256']==m['tools']['native']['sha256']
    baseline=load(root/'build/bo08-hua-sema-20261016/cases'/name/'result.json');source=Path(baseline['input_path']);assert digest(source)==baseline['input_sha256']==diagnostic['source_sha256']
    work=out/'cases'/name;work.mkdir(parents=True,exist_ok=False);trace=work/'gc.jsonl';rows={}
    for mode,exe in [('normal',root/'build/hua.exe'),('diagnostic',root/'build-a11-profile/hua_performance_probe.exe')]:
        artifact=work/(mode+'.huab')
        args=[exe,'run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact] if mode=='normal' else [exe,'--gc-cost-trace',trace,out/'compiler.huab',source,artifact]
        timing,stdout,stderr=command(root,out,name+'-'+mode,args,90 if mode=='normal' else 120)
        expected=(root/'build/bo09-vm-20261016/commands'/('real-final-'+name+'-counts.stdout')).read_bytes()
        assert stdout==expected and digest(artifact)==old['after']['normal']['artifact_sha256']
        if mode=='normal':assert json.loads(stderr)==old['budget'];counts=None;metrics=None
        else:
            counts,metrics=map(json.loads,stderr.decode('utf-8').splitlines());previous=diagnostic['counts']
            assert counts.keys()==previous.keys()
            for key in counts:
                if key!='gc_nanoseconds':assert counts[key]==previous[key],key
            assert metrics['steps']==old['budget']['steps'] and metrics['peak_calls']==old['budget']['peak_calls']
        rows[mode]={'timing':timing,'counts':counts,'metrics':metrics,'artifact_sha256':digest(artifact),'runtime_sha256':digest(exe)}
    original=[json.loads(line) for line in (root/'build/bo10-gc-20261017/cases'/name/'gc.jsonl').read_text(encoding='utf-8').splitlines()]
    actual=[json.loads(line) for line in trace.read_text(encoding='utf-8').splitlines()];assert len(original)==len(actual)==rows['diagnostic']['counts']['gc_collections']
    for a,b in zip(original,actual):equivalent(a,b)
    assert all(b['start_ns']>=a['start_ns']+a['total_ns'] for a,b in zip(actual,actual[1:]))
    phases={key:sum(row['phases_ns'][key] for row in actual)/1e9 for key in PHASES}
    for row in actual:row['known_scratch_capacity_bytes']=sum(row[k] for k in ['object_capacity_bytes','offsets_capacity_bytes','flat_edge_capacity_bytes','incoming_bytes','marked_capacity_bytes','work_capacity_bytes','index_bucket_lower_bytes'])
    normal=rows['normal']['timing'];before=old['after']['normal']['timing']
    data={'status':'passed','case':name,'rows':rows,'normal_before':before,'diagnostic_before':diagnostic['timing'],'diagnostic_gc_before':diagnostic['cost_total_seconds'],'diagnostic_phases_before':diagnostic['phases_seconds'],'diagnostic_phases_after':phases,'diagnostic_gc_after':sum(phases.values()),'ordinary_wall_change_percent':100*(normal['seconds']/before['seconds']-1),'ordinary_cpu_change_percent':100*(normal['cpu_seconds']/before['cpu_seconds']-1),'old_nested_growths':sum(x['adjacency_growths'] for x in original),'new_flat_growths':sum(x['flat_edge_growths'] for x in actual),'old_known_scratch_peak':diagnostic['peak_scratch_collection']['known_scratch_capacity_bytes'],'new_known_scratch_peak':max(x['known_scratch_capacity_bytes'] for x in actual),'peak_flat_collection':max(actual,key=lambda x:x['known_scratch_capacity_bytes']),'all_non_time_semantic_counters_per_collection_output_and_huab_equal':True,'timing_limit':'one local before/after, old normal record reused and nonadjacent; no stable or general speedup claim'}
    save(work/'result.json',data)
    print(json.dumps({k:data[k] for k in ['case','ordinary_cpu_change_percent','diagnostic_gc_before','diagnostic_gc_after','old_nested_growths','new_flat_growths','old_known_scratch_peak','new_known_scratch_peak']},ensure_ascii=False),flush=True)
    assert normal['cpu_seconds']<=before['cpu_seconds']*1.3+0.1,'STOP: unexplained ordinary CPU regression'
    return data

def analyze(root,out):
    current(root,out);cases=[load(out/'cases'/name/'result.json') for name in ['type_rules','ast_pool','parser']]
    c04=load(out/'validation/a07-c04/results.json');assert c04['status']=='passed' and c04['hua_sha256']==digest(root/'build/hua.exe')
    passed=re.findall(r'Test\s+#\d+: (\w+)\s+\.+\s+Passed',(out/'commands/affected-ctests.stdout').read_text(encoding='utf-8',errors='replace'));assert len(set(passed))==len(passed)==24
    commands=[load(p) for p in (out/'commands').glob('*.json')];assert all(x['returncode']==0 and not x['timeout'] for x in commands)
    assert all(x['rows']['normal']['runtime_sha256']==digest(root/'build/hua.exe') and x['rows']['diagnostic']['runtime_sha256']==digest(root/'build-a11-profile/hua_performance_probe.exe') for x in cases)
    result={'status':'BO-10 one-pass flat edge table locally accepted; GC optimization continues, A11 still pending','native_sha256':digest(root/'build/hua.exe'),'probe_sha256':digest(root/'build-a11-profile/hua_performance_probe.exe'),'graph_oracle':load(out/'graph-equivalence-final.json'),'cost_fixtures':17,'c04':{k:c04[k] for k in ['cases','checks','commands','seconds']},'distinct_ctests':passed,'cases':cases,'supervised_commands':len(commands),'supervised_wall_seconds':sum(x['seconds'] for x in commands),'timeouts':0,'nonzero':0,'source_modules':len(closure(root/'selfhost/archive_main.hua')),'compiler_bytes':sum(p.stat().st_size for p in closure(root/'selfhost/archive_main.hua').values()),'production_limits':[500000000,128,65536,600,16777216],'full_own_source_or_generations':False,'remaining':'Value/field enumeration and address hashing require current evidence before next separate change; no GC interval or lifetime shortcut'}
    result['nearby_old_ordinary_control']=load(out/'near-control.json')
    result['nested_collection_oracle']=load(out/'nested-equivalence.json')
    result['diagnostic_only_fix']=load(out/'diagnostic-review.json')
    save(out/'summary.json',result);print(json.dumps({k:result[k] for k in ['status','native_sha256','probe_sha256','c04','supervised_commands','timeouts']},ensure_ascii=False),flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['calibrate','run','analyze']);p.add_argument('--output',type=Path,required=True);p.add_argument('--case',choices=['type_rules','ast_pool','parser']);a=p.parse_args();r=Path(__file__).resolve().parents[1];o=a.output.resolve()
    if a.action=='calibrate':calibrate(r,o)
    elif a.action=='run':assert a.case;run_case(r,o,a.case)
    else:analyze(r,o)
if __name__=='__main__':main()
