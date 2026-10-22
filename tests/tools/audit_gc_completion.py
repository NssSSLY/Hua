"""BO-10 completion: metadata/visitor allocation, dynamic traversal sources,
native requested storage and session lifetime. No whole own-source or generations.
Evidence inputs and completed results are immutable and identity checked.
"""
from pathlib import Path
import argparse,json,re
from audit_bootstrap_phases import command,digest,save,closure,WATCHES
from audit_gc_cost import PHASES

def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def jsonlines(p):return [json.loads(line) for line in Path(p).read_text(encoding='utf-8').splitlines()]
def current(root,out):
    m=load(out/'manifest.json');assert digest(out/'compiler.huab')==m['compiler_sha256']
    assert {n:digest(p) for n,p in closure(root/'selfhost/archive_main.hua').items()}==m['compiler_sources']
    assert digest(out/'before/native.exe')==m['native_sha256'] and digest(out/'before/probe.exe')==m['probe_sha256']
    return m

def mechanism(root,out):
    groups={name:jsonlines(out/'commands'/(name+'.stdout')) for name in ['before-allocation-mechanism-r2','before-diagnostic-allocation-mechanism','after-allocation-mechanism','after-diagnostic-allocation-mechanism']}
    assert all(rows[-1]['status']=='passed' and rows[-1]['checks']==42 for rows in groups.values())
    old,diagnostic,new,new_diagnostic=groups.values()
    for a,d,b,e in zip(old[1:5],diagnostic[1:5],new[1:5],new_diagnostic[1:5]):
        n=a['vertices'];assert n==b['vertices']==d['vertices']==e['vertices']
        assert a['track_requested']*32==b['track_requested']*112 and a['track_live']*32==b['track_live']*112
        assert b==e and a['collect_calls']==b['collect_calls'] and a['collect_requested']==b['collect_requested']
        assert d['collect_calls']-a['collect_calls']==n and d['collect_requested']-a['collect_requested']==40*n
        assert a['collect_live_delta']==b['collect_live_delta']==0
    assert old[-2]==new[-2]
    save(out/'allocation-mechanism.json',{'status':'passed','old_record_bytes':112,'new_record_bytes':32,'ordinary_callback_allocations_per_vertex':0,'old_diagnostic_callback_extra_allocation_per_vertex':1,'new_diagnostic_callback_extra_allocation_per_vertex':0,'checks_per_build':42,'rows':groups,'limits':'Replacement new only; requested bytes exclude observer headers, allocator headers, C malloc and DLL allocations; diagnostic footprint/timing is not production.'})

DIFFERENT={'schema','start_ns','total_ns','phases_ns','record_capacity_bytes','index_build_probes','index_lookup_probes','index_max_build_probes','index_max_lookup_probes'}
def equivalent(a,b):
    assert a['schema']=='hua.gc.cost.v3' and b['schema']=='hua.gc.cost.v4'
    for k in a:
        if k not in DIFFERENT:assert a[k]==b[k],(k,a[k],b[k])
    assert a['record_capacity_bytes']*32==b['record_capacity_bytes']*112
    assert b['record_size_bytes']==32 and b['edge_visitor_builds']==1
    assert sum(b['phases_ns'].values())==b['total_ns'] and list(b['phases_ns'])==PHASES
    sources=b['value_sources'];assert len(sources)==4 and all(len(row)==21 for row in sources)
    assert [sum(row[i] for row in sources) for i in range(21)]==b['value_kinds']
    assert sum(sources[0])==b['struct_fields'] and sum(sources[1])==b['map_entries'] and sum(sources[2])==b['vector_values']
    assert b['index_build_probes']>=b['index_nodes'] and b['index_max_build_probes']<=b['index_nodes']
    assert b['index_lookup_probes']>=b['index_lookup_calls']-b['index_null_lookups'] if b['live_before'] else b['index_lookup_probes']==0

def calibrate(root,out):
    current(root,out);mechanism(root,out);old=load(root/'tests/artifacts/build/bo10-index-20261017/cost-equivalence.json')['rows'];actual={};name=None
    for row in jsonlines(out/'commands/cost-fixtures.stdout'):
        if 'fixture' in row:name=row['fixture']
        else:assert name not in actual;actual[name]=row
    assert actual.keys()==old.keys()
    for name,row in actual.items():equivalent(old[name],row)
    save(out/'cost-equivalence.json',{'status':'passed','fixtures':len(actual),'previous_semantic_fields_equal':True,'rows':actual})
    print('Allocation mechanism,17 source-partitioned graphs passed',flush=True)

def run_case(root,out,name):
    current(root,out);assert load(out/'cost-equivalence.json')['status']==load(out/'graph-equivalence.json')['status']=='passed'
    historical=load(root/'tests/artifacts/build/bo10-index-20261017/cases'/name/'result.json')
    source_info=load(root/'tests/artifacts/build/bo08-hua-sema-20261016/cases'/name/'result.json');source=Path(source_info['input_path']);assert digest(source)==source_info['input_sha256']
    work=out/'cases'/name;work.mkdir(parents=True,exist_ok=False);rows={};trace=work/'gc.jsonl'
    for mode,exe in [('before',out/'before/native.exe'),('after',root/'tests/artifacts/build/hua.exe'),('diagnostic',root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe')]:
        artifact=work/(mode+'.huab');args=[exe,'--gc-cost-trace',trace,out/'compiler.huab',source,artifact] if mode=='diagnostic' else [exe,'run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact]
        timing,stdout,stderr=command(root,out,name+'-'+mode,args,(120 if mode=='diagnostic' else 90) if name=='parser' else 30)
        assert stdout==(root/'tests/artifacts/build/bo10-index-20261017/commands'/(name+'-normal.stdout')).read_bytes()
        assert digest(artifact)==historical['rows']['normal']['artifact_sha256']
        counts=None;metrics=None
        if mode=='diagnostic':
            counts,metrics=map(json.loads,stderr.decode('utf-8').splitlines());prior=historical['rows']['diagnostic']
            assert counts.keys()==prior['counts'].keys()
            for key in counts:
                if key!='gc_nanoseconds':assert counts[key]==prior['counts'][key],key
            for key in metrics:
                if not key.endswith('_seconds'):assert metrics[key]==prior['metrics'][key],key
        else:assert json.loads(stderr)==json.loads((root/'tests/artifacts/build/bo10-index-20261017/commands'/(name+'-normal.stderr')).read_bytes())
        rows[mode]={'timing':timing,'runtime_sha256':digest(exe),'artifact_sha256':digest(artifact),'counts':counts,'metrics':metrics}
        if mode=='after':assert timing['cpu_seconds']<=rows['before']['timing']['cpu_seconds']*1.3+.1,'STOP: unexplained ordinary regression'
    old_trace=jsonlines(root/'tests/artifacts/build/bo10-index-20261017/cases'/name/'gc.jsonl');actual=jsonlines(trace);assert len(old_trace)==len(actual)==rows['diagnostic']['counts']['gc_collections']
    for a,b in zip(old_trace,actual):equivalent(a,b)
    assert all(b['start_ns']>=a['start_ns']+a['total_ns'] for a,b in zip(actual,actual[1:]))
    phases={k:sum(x['phases_ns'][k] for x in actual)/1e9 for k in PHASES}
    sources={label:[sum(x['value_sources'][i][j] for x in actual) for j in range(21)] for i,label in enumerate(['struct','map','sequence','environment-other'])}
    data={'status':'passed','case':name,'input_sha256':digest(source),'rows':rows,'diagnostic_phases_seconds':phases,'gc_seconds':sum(phases.values()),'value_sources':sources,'collections':len(actual),'max_record_capacity':max(x['record_capacity_bytes'] for x in actual),'old_record_capacity':max(x['record_capacity_bytes'] for x in old_trace),'peak_collection':max(actual,key=lambda x:x['live_before']),'ordinary_cpu_change_percent':100*(rows['after']['timing']['cpu_seconds']/rows['before']['timing']['cpu_seconds']-1),'all_prior_semantic_fields_per_round_output_huab_and_budget_equal':True,'address_probe_note':'Only pointer hash probes may vary with address/allocator placement; lookup count,logical graph,capacities remain compared.','timing_limit':'One before/after nearby ordinary pair. Current diagnostic also removes per-vertex observer allocations; phase reduction is not pure production speedup.'}
    save(work/'result.json',data);print(json.dumps({k:data[k] for k in ['case','ordinary_cpu_change_percent','gc_seconds','max_record_capacity']},ensure_ascii=False),flush=True)

def prepare_memory(root,out):
    m=current(root,out);target=out/'memory-source';target.mkdir()
    for name in m['compiler_sources']:
        text=(root/'selfhost'/name).read_text(encoding='utf-8')
        for signature,phase in WATCHES.get(name,[]):
            assert text.count(signature)==1,(name,signature)
            text=text.replace(signature,signature+'\n    print("@@hua.phase:begin:'+phase+'")\n    defer _bo_memory_end("'+phase+'")\n')
        text+='\nfn _bo_memory_end(label string) { print("@@hua.phase:end:"+label) }\n' if name in WATCHES else ''
        (target/name).write_text(text,encoding='utf-8',newline='\n')
    save(out/'memory-source.json',{'original_sources':m['compiler_sources'],'instrumented_sources':{p.name:digest(p) for p in target.glob('*.hua')},'scope':'diagnostic copies only; begin/deferred end. End marker precedes full caller-side retirement, not proof of last use.'})
    command(root,out,'build-memory-component',[root/'tests/artifacts/build/hua.exe','build',target/'archive_main.hua','-o',out/'memory-compiler.huab'],90)

def memory_case(root,out,name):
    current(root,out);target=out/'memory'/name;target.mkdir(parents=True,exist_ok=False)
    if name=='invalid':
        source=target/'invalid.hua';source.write_text('fn main(){print(missing)}\n',encoding='utf-8',newline='\n')
    else:source=Path(load(root/'tests/artifacts/build/bo08-hua-sema-20261016/cases'/name/'result.json')['input_path'])
    trace=target/'events.jsonl';artifact=target/'output.huab'
    timing,stdout,stderr=command(root,out,'memory-'+name,[root/'tests/artifacts/build-a11-profile/hua_memory_probe.exe','--phase-events',trace,out/'memory-compiler.huab',source,artifact],60)
    events=jsonlines(trace);assert events[0]['event']=='begin' and events[0]['phase']=='execution'
    assert events[-1]['event']=='end' and events[-1]['phase']=='heap-release'
    stack=[]
    for row in events:
        if row['event']=='begin':stack.append(row['phase'])
        else:assert stack.pop()==row['phase']
        assert row['native_memory']['live']<=row['native_memory']['peak']
    assert not stack
    finished=next(x for x in events if x['phase']=='execution' and x['event']=='end');assert finished['managed_live']==0
    final=events[-1];assert final['managed_live']==0
    if name=='invalid':assert json.loads(stdout)['status']=='error' and not artifact.exists()
    else:
        expected=load(root/'tests/artifacts/build/bo10-index-20261017/cases'/name/'result.json')['rows']['normal']['artifact_sha256']
        assert digest(artifact)==expected and stdout==(root/'tests/artifacts/build/bo10-index-20261017/commands'/(name+'-normal.stdout')).read_bytes()
    save(target/'result.json',{'status':'passed','case':name,'timing':timing,'memory_probe_sha256':digest(root/'tests/artifacts/build-a11-profile/hua_memory_probe.exe'),'instrumented_compiler_sha256':digest(out/'memory-compiler.huab'),'events':events,'managed_roots_at_session_end':finished['managed_live'],'peak_observed_managed_live':max(x['managed_live'] for x in events),'live_requested_at_execution_start':events[0]['native_memory']['live'],'live_requested_at_execution_end':finished['native_memory']['live'],'live_requested_after_heap_release':final['native_memory']['live'],'peak_requested':final['native_memory']['peak'],'requests_during_execution':finished['native_memory']['requested']-events[0]['native_memory']['requested'],'tracking_scope':'C++ replacement new requested sizes, including make_shared and owned backing; excludes audit prefix, malloc/DLL/OS and allocator headers. Extra headers/counters perturb placement and time. No ordinary speed claim.','phase_limit':'Snapshots at instrumented boundaries; no object-provenance/root dominator or early-reset proof. Final assertion after VM and resource scope retirement.'})
    print(name,'memory boundaries:',len(events),'final managed roots:',finished['managed_live'],flush=True)

def analyze(root,out):
    m=current(root,out);cases=[load(out/'cases'/n/'result.json') for n in ['type_rules','ast_pool','parser']]
    memory=[load(out/'memory'/n/'result.json') for n in ['type_rules','ast_pool','invalid']]
    c04=load(out/'validation/a07-c04/results.json');assert c04['status']=='passed' and c04['hua_sha256']==digest(root/'tests/artifacts/build/hua.exe')
    assert all(c['rows']['after']['runtime_sha256']==digest(root/'tests/artifacts/build/hua.exe') and c['rows']['diagnostic']['runtime_sha256']==digest(root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe') for c in cases)
    assert all(c['memory_probe_sha256']==digest(root/'tests/artifacts/build-a11-profile/hua_memory_probe.exe') and c['managed_roots_at_session_end']==0 for c in memory)
    passed=re.findall(r'Test\s+#\d+: (\w+)\s+\.+\s+Passed',(out/'commands/affected-ctests.stdout').read_text(encoding='utf-8'));expected=load(out/'regression-plan.json')['ctests'];assert set(passed)==set(expected) and len(passed)==len(expected)==30
    commands=[load(p) for p in (out/'commands').glob('*.json')];failed=[x['label'] for x in commands if x['returncode']!=0];assert set(failed)=={'before-allocation-mechanism','build-native-cache-probe'} and not any(x['timeout'] for x in commands)
    for c in memory:
        # Exclusive deltas; never sum nested live snapshots as a peak.
        stack=[];previous=None;by_phase={}
        for event in c['events']:
            if previous is not None and stack:
                row=by_phase.setdefault(stack[-1],{'requested_bytes':0,'allocation_calls':0,'wall_ns':0})
                for field,target in [('requested','requested_bytes'),('calls','allocation_calls')]:
                    delta=event['native_memory'][field]-previous['native_memory'][field];assert delta>=0;row[target]+=delta
                row['wall_ns']+=event['ns']-previous['ns']
            if event['event']=='begin':stack.append(event['phase'])
            else:assert stack.pop()==event['phase']
            previous=event
        assert not stack;c['exclusive_phase_native_allocations']=by_phase
        assert sum(x['requested_bytes'] for name,x in by_phase.items() if name!='heap-release')==c['requests_during_execution']
    parser=cases[-1];gc=parser['diagnostic_phases_seconds'];sample=load(out/'final-cpu-profile.json');assert sample['status']=='passed'
    scratch_fields=['object_capacity_bytes','offsets_capacity_bytes','flat_edge_capacity_bytes','incoming_bytes','marked_capacity_bytes','work_capacity_bytes','index_slot_bytes']
    rows=jsonlines(out/'cases/parser/gc.jsonl');scratch=max(sum(x[k] for k in scratch_fields) for x in rows)
    source_totals={name:sum(values) for name,values in parser['value_sources'].items()}
    scalar_indices=[0,1,2,3,4,15,18];scalar={name:sum(values[i] for i in scalar_indices) for name,values in parser['value_sources'].items()}
    ast=next(x for x in memory if x['case']=='ast_pool');phase_peak=max(ast['events'],key=lambda x:x['native_memory']['live'])
    decisions={
      'collector_metadata':'accepted: 112 to 32 bytes using stateless function pointers; all native consumers rebuilt',
      'visitor':'accepted: one non-owning synchronous visitor per collection; ordinary had no per-vertex heap allocation, old diagnostic did; both now same allocation counts',
      'dynamic_traversal':'completed source partition and ordinary CPU sample; retain all dynamic references/null/duplicates; no static declared-type shortcut',
      'cross_collection_scratch_reuse':{'disposition':'not adopted; evaluated, not outstanding','eligible_diagnostic_seconds':gc['graph_storage']+gc['scratch_teardown'],'eligible_percent_diagnostic_gc':100*(gc['graph_storage']+gc['scratch_teardown'])/sum(gc.values()),'retained_full_scratch_capacity_bytes':scratch,'reason':'Small remaining setup/destruction cost; clearing indexes/counts/pins still required. Retaining peak storage per heap increases lifetime/RSS, especially with Tasks.'},
      'arena_or_early_reset':{'disposition':'not adopted; conditional candidate evaluated, not a mandatory new language feature','observed_peak_phase':phase_peak['phase'],'observed_peak_live_requested':phase_peak['native_memory']['live'],'reason':'Peak during emit while Model/Core/Image are still needed. archive.encode uses checked Model and emitted Image; earlier reset has no lifetime proof. Releasing only before publication does not remove observed earlier peak. All three sessions end with no managed roots; existing graph/engine failure tests prove release.'},
      'advanced_gc':'No generational/moving/concurrent collector or write barrier required by this bounded work; HUA-D029 remains independent design.',
      'bo11':'Separate next stage: current-source scale prediction and margin before one formal own-source run. BO-10 completion is not A11/C07/C08 acceptance.'}
    result={'status':'BO-10 complete within Windows x64 Release Bridge OFF scope','acceptance':'Allocation/GC/lifetime tasks and original BO-10 exit contracts completed; conditional alternatives evaluated and rejected with evidence; no full own-source acceptance claimed.','native_sha256':digest(root/'tests/artifacts/build/hua.exe'),'probe_sha256':digest(root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe'),'memory_probe_sha256':digest(root/'tests/artifacts/build-a11-profile/hua_memory_probe.exe'),'compiler_sha256':m['compiler_sha256'],'compiler_sources':m['compiler_sources'],'compiler_bytes':sum(p.stat().st_size for p in closure(root/'selfhost/archive_main.hua').values()),'graph':load(out/'graph-equivalence.json'),'allocation':load(out/'allocation-mechanism.json'),'cost_fixtures':17,'ctests':passed,'c04':{k:c04[k] for k in ['cases','checks','commands','seconds']},'cases':cases,'memory':memory,'source_value_visits':source_totals,'source_scalar_visits':scalar,'parser_control':load(out/'parser-phase-control.json'),'ordinary_cpu_sample':sample,'conditional_decisions':decisions,'native_persistent_objects':load(out/'native-persistent-check.json'),'supervised_commands':len(commands),'supervised_wall_seconds':sum(x['seconds'] for x in commands),'timeouts':0,'nonzero_commands':failed,'audit_stops':['Original test fixture omitted clear; corrected before production changes.','Initial standalone cache probe link omitted frontend dependency; corrected without repeating Hua workload.','All 12968 residual bytes were initially hypothesized to be builtin_names; independent probe measured only9840, hypothesis rejected. Scheduler/persistent/trace accounting kept separate, not claimed byte-exact.'],'production_limits':[500000000,128,65536,600,16777216],'full_own_source_or_generations':False,'global_completion':{'BO-10':True,'A11':False,'BO-11':False,'C07_own_source':False,'C08':False},'scope_limits':'No Bridge ON/POSIX/other architecture/full suite evidence; native allocation audit excludes malloc/DLL/OS/header bytes and perturbs allocation placement.'}
    save(out/'summary.json',result)
    print(json.dumps({k:result[k] for k in ['status','native_sha256','probe_sha256','memory_probe_sha256','supervised_commands','nonzero_commands','timeouts']},ensure_ascii=False),flush=True)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('action',choices=['calibrate','run','prepare-memory','memory','analyze']);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--case',choices=['type_rules','ast_pool','parser','invalid']);a=ap.parse_args();r=Path(__file__).resolve().parents[2];o=a.output.resolve()
    if a.action=='analyze':analyze(r,o)
    elif a.action=='calibrate':calibrate(r,o)
    elif a.action=='prepare-memory':prepare_memory(r,o)
    elif a.action=='memory':assert a.case;memory_case(r,o,a.case)
    else:assert a.case in ['type_rules','ast_pool','parser'];run_case(r,o,a.case)
if __name__=='__main__':main()
