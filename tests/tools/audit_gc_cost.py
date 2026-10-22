"""BO-10 bounded current-thread GC cost diagnosis, never a full own-source gate.
Initial frozen identities and BO-09 baseline live in a separately created manifest.
Real inputs run once; completed command labels cannot be overwritten.
"""
from pathlib import Path
import argparse,json,sys
from audit_bootstrap_phases import command,digest,save,closure
PHASES=['prune_expired','pin_and_index','graph_storage','enumerate_edges','discover_roots','mark_reachable','clear_unreachable','release_and_prune','scratch_teardown']
VALUE_KINDS=['nil','bool','int','float','string','slice','struct','callable','map','multi','result','json','bytes','buffer','list','numeric','task','error','void','file','iofailure']

def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def verify(row):
    assert list(row['phases_ns'])==PHASES and sum(row['phases_ns'].values())==row['total_ns']
    assert row['records_before']==row['live_before']+row['expired_before']
    assert row['live_before']==row['records_after']+row['expired_after']
    assert row['emitted_edges']==row['tracked_edges']+row['untracked_edges'] and row['null_edges']<=row['untracked_edges']
    assert row['roots']<=row['marked']<=row['live_before']
    assert row['live_before']-row['marked']==row['clearable_unreachable']+row['unclearable_unreachable']
    assert len(row['value_kinds'])==len(VALUE_KINDS)
    assert row['index_nodes']==row['live_before']
    assert row['adjacency_nonempty']<=row['live_before'] and row['adjacency_growths']>=row['adjacency_nonempty']

def calibrate(root,out):
    raw=(out/'commands/calibrate-graph-facts.stdout').read_text(encoding='utf-8').splitlines();rows={};name=None;checks=0
    for line in raw:
        row=json.loads(line)
        if 'fixture' in row:name=row['fixture'];assert name not in rows
        else:verify(row);rows[name]=row;checks+=9
    assert len(rows)==17
    def check(name,**fields):
        nonlocal checks
        for k,v in fields.items():assert rows[name][k]==v,(name,k,rows[name][k],v);checks+=1
    for name in ('empty','restored-scope'):check(name,records_before=0,live_before=0,roots=0,tracked_edges=0)
    for n,name in [(64,'expired64'),(16,'released-chain16'),(4096,'sparse-released4096'),(65536,'expired65536')]:check(name,records_before=n,expired_before=n,live_before=0,records_after=0)
    check('live-chain16',records_before=16,live_before=16,tracked_edges=15,roots=1,marked=16,records_after=16,adjacency_nonempty=15,adjacency_growths=15,object_growths=5)
    for n,repeats in [(4,2),(256,4),(4096,4)]:
        check('live-ring-'+str(n)+'-'+str(repeats),live_before=n,tracked_edges=n*repeats,roots=1,marked=n,clearable_unreachable=0,records_after=n,adjacency_nonempty=n,adjacency_growths=n*(2 if repeats==2 else 3))
        check('dead-ring-'+str(n)+'-'+str(repeats),live_before=n,tracked_edges=n*repeats,roots=0,marked=0,clearable_unreachable=n,records_after=0,expired_after=n)
    check('foreign-holder',live_before=1,emitted_edges=2,tracked_edges=0,null_edges=1,untracked_edges=2,roots=1,marked=1,records_after=1)
    check('foreign-released',records_before=1,expired_before=1,live_before=0,records_after=0)
    check('value-kinds',live_before=2,tracked_edges=1,roots=2,marked=2,struct_scans=1,struct_fields=5,list_scans=1,vector_scans=1,vector_values=4)
    want=[0]*21;want[2]=7;want[4]=1;want[6]=1;assert rows['value-kinds']['value_kinds']==want;checks+=1
    check('sparse-roots4096',live_before=4096,tracked_edges=0,roots=4096,marked=4096,records_after=4096,adjacency_growths=0,object_growths=13,work_growths=13)
    save(out/'calibration.json',{'status':'passed','fixtures':17,'independent_graph_checks':checks,'native_fixture_checks':(out/'commands/calibrate-graph-facts.stderr').read_text().strip(),'duplicates':'reference multiplicity retained, tested 2 and4 references to same target','scope':'external foreign holders, null/untracked edges, immediate RC release, cycles, nested recorder; wall-clock phases have no independent timing oracle','rows':rows})
    print('GC calibration:',checks,'independent facts',flush=True)

def current(root,out):
    m=load(out/'manifest.json');assert {n:digest(p) for n,p in closure(root/'selfhost/archive_main.hua').items()}==m['compiler_sources']
    assert digest(root/'tests/artifacts/build/hua.exe')==m['tools']['native']['sha256'] and digest(out/'compiler.huab')==m['compiler_sha256']
    return m

def run_case(root,out,name):
    m=current(root,out);assert load(out/'calibration.json')['status']=='passed'
    before=load(root/'tests/artifacts/build/bo09-vm-20261016/real-final/results.json');old=next(r for r in before['rows'] if r['case']==name)
    exe=root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe';work=out/'cases'/name;work.mkdir(parents=True,exist_ok=True);assert not any(work.iterdir()),'refuse to overwrite prior case evidence'
    source=Path(old['after']['normal']['timing']['command'][-2]);assert source.is_file()
    # Original BO-09 validated these source identities and full component closure.
    prior=load(root/'tests/artifacts/build/bo08-hua-sema-20261016/cases'/name/'result.json');assert source==Path(prior['input_path']) and digest(source)==prior['input_sha256']
    trace=work/'gc.jsonl';artifact=work/'output.huab';args=[exe,'--gc-cost-trace',trace,out/'compiler.huab',source,artifact]
    timing,stdout,stderr=command(root,out,'gc-'+name,args,120 if name=='parser' else 30)
    rows=[json.loads(x) for x in trace.read_text(encoding='utf-8').splitlines()];counts,metrics=map(json.loads,stderr.decode('utf-8').splitlines())
    prior_counts=old['after']['counts']['counts'];assert counts.keys()==prior_counts.keys()
    assert {k:v for k,v in counts.items() if k!='gc_nanoseconds'}=={k:v for k,v in prior_counts.items() if k!='gc_nanoseconds'}
    expected=Path(root/'tests/artifacts/build/bo09-vm-20261016/commands'/('real-final-'+name+'-counts.stdout')).read_bytes()
    assert stdout==expected and digest(artifact)==old['after']['counts']['artifact_sha256']
    assert metrics['steps']==old['budget']['steps'] and metrics['peak_calls']==old['budget']['peak_calls']
    assert len(rows)==counts['gc_collections'] and len(rows)<4096
    for i,row in enumerate(rows):verify(row);assert row['ordinal']==i+1
    assert sum(row['records_before'] for row in rows)==counts['gc_records']
    assert sum(row['live_before'] for row in rows)==counts['gc_live_records']
    assert sum(row['tracked_edges'] for row in rows)==counts['gc_edges']
    assert rows[-1]['allocated']==counts['managed_allocations']
    phases={k:sum(row['phases_ns'][k] for row in rows)/1e9 for k in PHASES}
    value_kinds={k:sum(row['value_kinds'][i] for row in rows) for i,k in enumerate(VALUE_KINDS)}
    fields=['records_before','expired_before','live_before','emitted_edges','tracked_edges','untracked_edges','null_edges','roots','external_excess','marked','clearable_unreachable','unclearable_unreachable','records_after','expired_after','object_growths','index_rehashes','adjacency_growths','adjacency_nonempty','work_growths','struct_scans','struct_fields','map_scans','map_entries','list_scans','multi_scans','vector_scans','vector_values']
    totals={k:sum(row[k] for row in rows) for k in fields}
    memory_keys=['object_capacity_bytes','adjacency_outer_bytes','adjacency_capacity_bytes','incoming_bytes','marked_capacity_bytes','work_capacity_bytes','index_bucket_lower_bytes']
    for row in rows:row['known_scratch_capacity_bytes']=sum(row[k] for k in memory_keys)
    peak=max(rows,key=lambda row:row['known_scratch_capacity_bytes']);live=max(rows,key=lambda row:row['live_before'])
    baseline=old['after']['counts']['timing'];ratio=timing['cpu_seconds']/baseline['cpu_seconds']
    data={'status':'passed','case':name,'source_sha256':digest(source),'probe_sha256':digest(exe),'artifact_sha256':digest(artifact),'timing':timing,'old_counter_probe_timing':baseline,'cpu_ratio_to_counter_only':ratio,'counts':counts,'metrics':metrics,'phases_seconds':phases,'cost_total_seconds':sum(phases.values()),'totals':totals,'value_kinds':value_kinds,'peak_live_collection':live,'peak_scratch_collection':peak,'repeat_scan_lower_bound':max(0,totals['live_before']-counts['managed_allocations']),'record_rows':len(rows),'all_non_time_counters_output_huab_and_budget_equal':True,'memory_limit':'known vector capacity bytes plus bucket pointer lower bound only; excludes index nodes, allocator headers, object payload and most live backing storage; not RSS','time_limit':'diagnostic stages include bookkeeping; trace I/O occurs after collection timer, whole run includes it; not OFF CPU shares'}
    save(work/'result.json',data)
    print(json.dumps({k:data[k] for k in ('case','record_rows','cost_total_seconds','phases_seconds','cpu_ratio_to_counter_only')},ensure_ascii=False),flush=True)
    # This is a diagnostic-overhead gate, not a production speed threshold.
    assert timing['cpu_seconds']<=baseline['cpu_seconds']*1.35+0.1,'STOP: profiling overhead excessive; do not enlarge input'
    return data

def analyze(root,out):
    m=current(root,out);assert load(out/'off-preprocessor.json')['status']=='passed';cal=load(out/'calibration.json')
    cases=[load(out/'cases'/name/'result.json') for name in ('type_rules','ast_pool','parser')]
    assert all(c['status']=='passed' for c in cases)
    for c in cases:
        rows=[json.loads(line) for line in (out/'cases'/c['case']/'gc.jsonl').read_text(encoding='utf-8').splitlines()]
        assert all(b['start_ns']>=a['start_ns']+a['total_ns'] for a,b in zip(rows,rows[1:])), 'nested GC time would overlap; do not sum'
        assert all(row['marked']==row['live_before'] and row['clearable_unreachable']==row['unclearable_unreachable']==0 for row in rows)
        scalar=sum(c['value_kinds'][k] for k in ('nil','bool','int','float','string','numeric','void'))
        c['derived']={'phase_percent_of_diagnostic_gc':{k:100*v/c['cost_total_seconds'] for k,v in c['phases_seconds'].items()},'scalar_value_visits':scalar,'all_value_visits':sum(c['value_kinds'].values()),'scalar_value_percent':100*scalar/sum(c['value_kinds'].values()),'final_records':rows[-1]['records_after'],'no_nested_collection_overlap':True,'all_live_records_reachable':True}
    commands=[load(p) for p in (out/'commands').glob('*.json')];assert all(c['returncode']==0 and not c['timeout'] for c in commands)
    save(out/'summary.json',{'status':'BO-10 diagnostic cost decomposition complete; no production optimization or A11 acceptance','baseline_git':m['git'],'ordinary_runtime_sha256':m['tools']['native']['sha256'],'counter_only_probe_sha256':m['tools']['probe']['sha256'],'gc_cost_probe_sha256':digest(root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe'),'compiler_sha256':m['compiler_sha256'],'compiler_modules':len(m['compiler_sources']),'compiler_source_bytes':sum(p.stat().st_size for p in closure(root/'selfhost/archive_main.hua').values()),'calibration_checks':cal['independent_graph_checks'],'cases':cases,'commands':len(commands),'timeouts':0,'nonzero_commands':0,'production_algorithm_or_budget_changed':False,'full_own_source_or_generations':False})
    print('BO-10 cost summary passed; no production change',flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['calibrate','run','analyze']);p.add_argument('--output',type=Path,required=True);p.add_argument('--case',choices=['type_rules','ast_pool','parser']);a=p.parse_args();r=Path(__file__).resolve().parents[2];o=a.output.resolve()
    if a.action=='calibrate':calibrate(r,o)
    elif a.action=='run':assert a.case;run_case(r,o,a.case)
    else:analyze(r,o)
if __name__=='__main__':main()
