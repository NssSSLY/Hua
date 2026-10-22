"""BO-10 collection-local contiguous address index. Frozen flat-index baseline;
one bounded run per real input and build mode; no own-source or generations.
"""
from pathlib import Path
import argparse,json,re
from audit_bootstrap_phases import command,digest,save,closure
from audit_gc_cost import PHASES

def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def baseline(root):return root/'tests/artifacts/build/bo10-flat-20261017'
def current(root,out):
    m=load(out/'manifest.json');assert digest(out/'compiler.huab')==m['compiler_sha256']
    assert {n:digest(p) for n,p in closure(root/'selfhost/archive_main.hua').items()}==m['compiler_sources']
    assert digest(root/'tests/reference/gc_nested_edges.cpp')==m['files']['tests/reference/gc_nested_edges.cpp']
    for mode in ['native','probe']:assert digest(Path(m['tools'][mode]['path']))==m['tools'][mode]['sha256']
    return m

# Map buckets/node storage and contiguous slots are different representation units.
DIFFERENT={'schema','start_ns','total_ns','phases_ns','index_rehashes','index_buckets','index_bucket_lower_bytes'}
def equivalent(before,after):
    assert before['schema']=='hua.gc.cost.v2' and after['schema']=='hua.gc.cost.v3'
    for key in before:
        if key not in DIFFERENT:assert before[key]==after[key],(key,before[key],after[key])
    assert list(after['phases_ns'])==PHASES and sum(after['phases_ns'].values())==after['total_ns']
    assert after['index_rehashes']==after['index_buckets']==after['index_bucket_lower_bytes']==0
    n=after['live_before'];slots=after['index_slots']
    assert slots==0 if n==0 else slots>=max(8,2*n) and slots&(slots-1)==0
    assert after['index_slot_bytes']==slots*16 and after['index_buffer_builds']==bool(n)
    assert after['index_nodes']<=n and after['index_build_probes']>=n
    assert after['index_lookup_calls']==after['emitted_edges'] and after['index_null_lookups']==after['null_edges']
    assert after['index_lookup_probes']>=after['index_lookup_calls']-after['index_null_lookups'] if n else after['index_lookup_probes']==0
    assert after['index_max_build_probes']<=after['index_nodes'] and after['index_max_lookup_probes']<=after['index_nodes']+1

def calibrate(root,out):
    current(root,out);old=load(baseline(root)/'cost-equivalence.json')['rows'];rows={};name=None
    for line in (out/'commands/cost-fixtures.stdout').read_text(encoding='utf-8').splitlines():
        row=json.loads(line)
        if 'fixture' in row:name=row['fixture']
        else:assert name not in rows;rows[name]=row
    assert rows.keys()==old.keys()
    for name,row in rows.items():equivalent(old[name],row)
    oracle=[json.loads(line) for line in (out/'commands/index-oracle.stdout').read_text(encoding='utf-8').splitlines()]
    assert oracle[-1]['status']=='passed'
    save(out/'cost-equivalence.json',{'status':'passed','fixtures':len(rows),'all_previous_semantic_fields_equal':True,'rows':rows})
    save(out/'index-equivalence.json',{'status':'passed','tests':oracle[-1],'allocations':oracle[:-1],'allocation_limit':'Cumulative requested bytes only, excludes allocator metadata and user payload; not RSS or peak live allocation.'})
    print('17 graph diagnostic fixtures and independent address index oracle passed',flush=True)

def ordinary_control(root,out,name):
    m=current(root,out);prior=baseline(root)/'commands'/(name+'-normal');work=out/'controls';work.mkdir(exist_ok=True)
    if name=='type_rules':
        timing=load(out/'commands/type-rules-old-control.json');artifact=out/'control-type-rules.huab'
        stdout=(out/'commands/type-rules-old-control.stdout').read_bytes();stderr=(out/'commands/type-rules-old-control.stderr').read_bytes()
    else:
        source=Path(load(root/'tests/artifacts/build/bo08-hua-sema-20261016/cases'/name/'result.json')['input_path']);artifact=work/(name+'.huab')
        timing,stdout,stderr=command(root,out,name+'-old-control',[out/'before/native.exe','run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact],90 if name=='parser' else 30)
    assert stdout==prior.with_suffix('.stdout').read_bytes() and stderr==prior.with_suffix('.stderr').read_bytes()
    assert digest(artifact)==load(baseline(root)/'cases'/name/'result.json')['rows']['normal']['artifact_sha256']
    save(work/(name+'.json'),{'timing':timing,'artifact_sha256':digest(artifact),'runtime_sha256':m['tools']['native']['sha256'],'full_outputs_budget_and_artifact_equal':True})

def run_case(root,out,name):
    m=current(root,out);assert load(out/'graph-equivalence.json')['status']==load(out/'cost-equivalence.json')['status']=='passed'
    old=load(baseline(root)/'cases'/name/'result.json')
    assert old['rows']['normal']['runtime_sha256']==m['tools']['native']['sha256']
    assert old['rows']['diagnostic']['runtime_sha256']==m['tools']['probe']['sha256']
    source_info=load(root/'tests/artifacts/build/bo08-hua-sema-20261016/cases'/name/'result.json');source=Path(source_info['input_path']);assert digest(source)==source_info['input_sha256']
    work=out/'cases'/name
    if name=='type_rules':assert work.exists() and not (work/'result.json').exists()
    else:work.mkdir(parents=True,exist_ok=False)
    trace=work/'gc.jsonl';rows={};control=load(out/'controls'/(name+'.json'))
    for mode,exe in [('normal',root/'tests/artifacts/build/hua.exe'),('diagnostic',root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe')]:
        artifact=work/(mode+'.huab')
        args=[exe,'run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact] if mode=='normal' else [exe,'--gc-cost-trace',trace,out/'compiler.huab',source,artifact]
        if name=='type_rules' and mode=='normal':
            timing=load(out/'commands/type_rules-normal.json');assert timing['command']==list(map(str,args)) and timing['returncode']==0 and not timing['timeout']
            stdout=(out/'commands/type_rules-normal.stdout').read_bytes();stderr=(out/'commands/type_rules-normal.stderr').read_bytes()
        else:timing,stdout,stderr=command(root,out,name+'-'+mode,args,(90 if mode=='normal' else 120) if name=='parser' else 30)
        assert stdout==(baseline(root)/'commands'/(name+'-'+mode+'.stdout')).read_bytes()
        assert digest(artifact)==old['rows'][mode]['artifact_sha256']
        if mode=='normal':
            assert json.loads(stderr)==json.loads((baseline(root)/'commands'/(name+'-normal.stderr')).read_bytes());counts=None;metrics=None
            assert timing['cpu_seconds']<=control['timing']['cpu_seconds']*1.3+0.1,'STOP: ordinary CPU regression'
        else:
            counts,metrics=map(json.loads,stderr.decode('utf-8').splitlines());previous=old['rows'][mode]['counts'];assert counts.keys()==previous.keys()
            for key in counts:
                if key!='gc_nanoseconds':assert counts[key]==previous[key],key
            previous_metrics=old['rows'][mode]['metrics'];assert metrics.keys()==previous_metrics.keys()
            for key in metrics:
                if not key.endswith('_seconds'):assert metrics[key]==previous_metrics[key],key
        rows[mode]={'timing':timing,'counts':counts,'metrics':metrics,'artifact_sha256':digest(artifact),'runtime_sha256':digest(exe)}
    original=[json.loads(line) for line in (baseline(root)/'cases'/name/'gc.jsonl').read_text(encoding='utf-8').splitlines()]
    actual=[json.loads(line) for line in trace.read_text(encoding='utf-8').splitlines()];assert len(original)==len(actual)==rows['diagnostic']['counts']['gc_collections']
    for a,b in zip(original,actual):equivalent(a,b)
    assert all(b['start_ns']>=a['start_ns']+a['total_ns'] for a,b in zip(actual,actual[1:]))
    phases={key:sum(row['phases_ns'][key] for row in actual)/1e9 for key in PHASES}
    common=['object_capacity_bytes','offsets_capacity_bytes','flat_edge_capacity_bytes','incoming_bytes','marked_capacity_bytes','work_capacity_bytes']
    peak=max(actual,key=lambda x:sum(x[k] for k in common)+x['index_slot_bytes'])
    index_fields=['index_nodes','index_slots','index_slot_bytes','index_buffer_builds','index_build_probes','index_lookup_probes','index_lookup_calls','index_null_lookups']
    data={'status':'passed','case':name,'source_path':str(source),'source_sha256':digest(source),'rows':rows,'before':old['rows'],'nearby_old_ordinary_control':control,'phases_before':old['diagnostic_phases_after'],'phases_after':phases,'gc_before':old['diagnostic_gc_after'],'gc_after':sum(phases.values()),'index_totals':{k:sum(x[k] for x in actual) for k in index_fields},'old_index_rehashes':sum(x['index_rehashes'] for x in original),'max_build_probes':max(x['index_max_build_probes'] for x in actual),'max_lookup_probes':max(x['index_max_lookup_probes'] for x in actual),'old_common_scratch_plus_buckets_peak':max(sum(x[k] for k in common)+x['index_bucket_lower_bytes'] for x in original),'new_common_scratch_plus_slots_peak':sum(peak[k] for k in common)+peak['index_slot_bytes'],'peak_collection':peak,'capacity_limit':'Old partial buffer accounting excludes per-node allocations; new contiguous slot buffer is exact but excludes allocator metadata, records and user payload. Compare process peak separately.','ordinary_cpu_change_percent':100*(rows['normal']['timing']['cpu_seconds']/control['timing']['cpu_seconds']-1),'ordinary_wall_change_percent':100*(rows['normal']['timing']['seconds']/control['timing']['seconds']-1),'all_prior_semantic_counters_per_collection_output_huab_and_budgets_equal':True,'timing_limit':'One candidate run per mode/input; the initial completed small run was verified and reused without rerun. One nearby frozen old ordinary control per input after historical small CPU mismatch; historical diagnostic times are not comparable until explicit fresh control. No stable/general speedup claim.'}
    save(work/'result.json',data)
    print(json.dumps({k:data[k] for k in ['case','ordinary_cpu_change_percent','gc_before','gc_after','max_build_probes','max_lookup_probes']},ensure_ascii=False),flush=True)
    return data

def parser_diagnostic_control(root,out):
    name='parser';work=out/'controls';trace=work/'parser-gc.jsonl';source=Path(load(root/'tests/artifacts/build/bo08-hua-sema-20261016/cases'/name/'result.json')['input_path']);artifact=work/'parser-diagnostic.huab'
    timing,stdout,stderr=command(root,out,'parser-old-diagnostic-control',[out/'before/probe.exe','--gc-cost-trace',trace,out/'compiler.huab',source,artifact],120)
    expected=load(baseline(root)/'cases/parser/result.json');assert stdout==(baseline(root)/'commands/parser-diagnostic.stdout').read_bytes() and digest(artifact)==expected['rows']['diagnostic']['artifact_sha256']
    counts,metrics=map(json.loads,stderr.decode('utf-8').splitlines())
    for key in counts:
        if key!='gc_nanoseconds':assert counts[key]==expected['rows']['diagnostic']['counts'][key],key
    for key in metrics:
        if not key.endswith('_seconds'):assert metrics[key]==expected['rows']['diagnostic']['metrics'][key],key
    old=[json.loads(line) for line in trace.read_text(encoding='utf-8').splitlines()];new=[json.loads(line) for line in (out/'cases/parser/gc.jsonl').read_text(encoding='utf-8').splitlines()]
    assert len(old)==len(new)
    for a,b in zip(old,new):equivalent(a,b)
    data=load(out/'cases/parser/result.json');data['historical_phases_before']=data['phases_before'];data['historical_gc_before']=data['gc_before']
    data['phases_before']={k:sum(x['phases_ns'][k] for x in old)/1e9 for k in PHASES};data['gc_before']=sum(data['phases_before'].values())
    data['nearby_old_diagnostic_control']={'timing':timing,'runtime_sha256':digest(out/'before/probe.exe'),'full_outputs_artifact_counters_and_rounds_equal':True}
    save(out/'cases/parser/result.json',data)

def analyze(root,out):
    m=current(root,out);cases=[load(out/'cases'/name/'result.json') for name in ['type_rules','ast_pool','parser']]
    c04=load(out/'validation/a07-c04/results.json');assert c04['status']=='passed' and c04['hua_sha256']==digest(root/'tests/artifacts/build/hua.exe')
    passed=re.findall(r'Test\s+#\d+: (\w+)\s+\.+\s+Passed',(out/'commands/affected-ctests.stdout').read_text(encoding='utf-8',errors='replace'));assert len(set(passed))==len(passed)==24
    commands=[load(p) for p in (out/'commands').glob('*.json')];assert all(x['returncode']==0 and not x['timeout'] for x in commands)
    assert all(x['rows']['normal']['runtime_sha256']==digest(root/'tests/artifacts/build/hua.exe') and x['rows']['diagnostic']['runtime_sha256']==digest(root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe') for x in cases)
    data={'status':'BO-10 contiguous address index locally accepted; A11 and further GC optimization pending','native_sha256':digest(root/'tests/artifacts/build/hua.exe'),'probe_sha256':digest(root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe'),'graph_oracle':load(out/'graph-equivalence.json'),'address_oracle':load(out/'index-equivalence.json'),'cost_fixtures':17,'c04':{k:c04[k] for k in ['cases','checks','commands','seconds']},'distinct_ctests':passed,'cases':cases,'cpu_sample':load(out/'cpu-profile.json'),'initial_performance_stop':load(out/'regression-stop.json'),'small_control':load(out/'type-rules-control.json'),'supervised_commands':len(commands),'supervised_wall_seconds':sum(x['seconds'] for x in commands),'timeouts':0,'nonzero':0,'compiler_sha256':m['compiler_sha256'],'compiler_source_modules':len(m['compiler_sources']),'compiler_source_bytes':sum(p.stat().st_size for p in closure(root/'selfhost/archive_main.hua').values()),'production_limits':[500000000,128,65536,600,16777216],'full_own_source_or_generations':False,'remaining':'Classify remaining Value/Struct field enumeration and repeated scalar visits before changing traversal; no interval, root or lifetime shortcut.'}
    save(out/'summary.json',data)
    print(json.dumps({k:data[k] for k in ['status','native_sha256','probe_sha256','c04','supervised_commands','timeouts']},ensure_ascii=False),flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['calibrate','run','analyze']);p.add_argument('--output',type=Path,required=True);p.add_argument('--case',choices=['type_rules','ast_pool','parser']);a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve()
    if a.action=='calibrate':calibrate(root,out)
    elif a.action=='run':assert a.case;run_case(root,out,a.case)
    else:analyze(root,out)
if __name__=='__main__':main()
