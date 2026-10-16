"""BO-08 host semantic copy/iteration evidence; never executes the Hua compiler VM."""
from pathlib import Path
import argparse,json,os,subprocess,hashlib
from audit_bootstrap_phases import command,save,digest,closure

def scalar(n,b,logic=False):
    params='flag bool' if logic else ''
    expression='flag && flag' if logic else 'v0+v'+str(n-1)
    return 'fn work('+params+') int{\n'+''.join(f'var v{i}={i}\n' for i in range(n))+(''+expression+'\n')*b+'return v0\n}\nfn main(){}\n'
def wide(w):
    fields='\n'.join(f'f{i} Leaf' for i in range(w));value='Box{'+','.join(f'f{i}:Leaf{{xs:[1]}}' for i in range(w))+'}'
    return 'struct Leaf{xs []int}\nstruct Box{'+fields+'}\nfn work() int{\n'+''.join(f'var v{i}={value}\n' for i in range(4))+('v0.f0.xs[0]+v3.f0.xs[0]\n')*4+'return 0}\nfn main(){}\n'
def initialize(root,out):
    assert (out/'plan.json').exists() and not (out/'manifest.json').exists();target=out/'inputs';target.mkdir();rows=[]
    def add(name,text,**meta):
        p=target/(name+'.hua');p.write_text(text,encoding='utf-8',newline='\n');rows.append({'name':name,'path':str(p),'sha256':digest(p),'bytes':p.stat().st_size,**meta})
    add('calibration-arithmetic',scalar(2,1),kind='scalar',n=2,b=1,f=2)
    add('calibration-logic',scalar(2,1,True),kind='logic',n=2,b=1,f=2)
    for n,b in [(8,4),(16,4),(32,4),(16,1),(16,8),(20,6)]:add(f'scalar-n{n}-b{b}',scalar(n,b),kind='scalar',n=n,b=b,f=2)
    add('logic-n16-b4',scalar(16,4,True),kind='logic',n=16,b=4,f=2)
    for f in (8,16,32):add(f'functions-{f}',''.join(f'fn f{i}() int{{return 1+2}}\n' for i in range(f))+'fn main(){}\n',kind='functions',f=f+1,n=0,b=1)
    for w in (1,4,8):add(f'views-w{w}',wide(w),kind='views',w=w)
    add('dependencies-8','struct Box{xs []int}\n'+''.join(f'fn f{i}(b mut Box) Box{{return '+(f'f{i+1}(b)' if i<7 else 'b')+'}\n' for i in range(8))+'fn main(){}\n',kind='dependencies')
    add('aliases-16','fn work(){var a=[1]\n'+''.join(f'var v{i}=a\n' for i in range(16))+'a[0]=2\n}\nfn main(){}\n',kind='aliases')
    for name in ('catalog','type_rules'):
        p=root/'build/bo03-20261016/inputs'/(name+'.hua');add(name,p.read_text(encoding='utf-8'),kind='real')
    p=root/'selfhost/parser_extended_main.hua';sources=closure(p);rows.append({'name':'parser-closure','path':str(p),'sha256':digest(p),'bytes':sum(v.stat().st_size for v in sources.values()),'kind':'closure','modules':{k:digest(v) for k,v in sources.items()}})
    tools={'native':root/'build/hua.exe','probe':root/'build-bo08-sema-20261016/hua_sema_scale_probe.exe','guarded-cli':root/'build-bo08-sema-20261016/hua.exe'}
    save(out/'manifest.json',{'tools':{k:{'path':str(v),'sha256':digest(v)} for k,v in tools.items()},'inputs':rows,'area_order':['other','binary_eager','binary_logic','branch_snapshot','loop_snapshot','scope_restore','global_snapshot','function_reset','summary_snapshot','summary_store','join','fields','alias','exit','returns','apply','closure','call_actuals','unknown'],'phase_order':['setup','summaries','checking'],'counter_semantics':'actual copy construction/assignment; no moves, no bytes/allocator/CPU share claim','full_own_source_run':False})

def verify(root,out):
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'))
    for t in m['tools'].values():assert digest(t['path'])==t['sha256'],t
    for case in m['inputs']:assert digest(case['path'])==case['sha256'],case
    return m

def run_case(root,out,c,disabled=False,limit=30):
    m=verify(root,out);target=out/c['name'];target.mkdir(exist_ok=True);destination=target/'result.json';assert not destination.exists();rows={}
    normal=target/'normal.huab';timing,stdout,stderr=command(root,out,c['name']+'-native',[m['tools']['native']['path'],'build',c['path'],'-o',normal],limit=limit);rows['normal']=timing
    artifacts=[];facts=[]
    for mode in (('disabled','enabled') if disabled else ('enabled',)):
        artifact=target/(mode+'.huab');counts=target/(mode+'-counts.json');stages=target/(mode+'-stages.jsonl');fact=target/(mode+'-facts.json')
        timing,stdout,stderr=command(root,out,c['name']+'-'+mode,[m['tools']['probe']['path'],counts,stages,fact,c['path'],artifact,mode],limit=limit)
        assert artifact.read_bytes()==normal.read_bytes(),(c['name'],'HUAB mismatch')
        count=json.loads(counts.read_text(encoding='utf-8'));factdata=json.loads(fact.read_text(encoding='utf-8'));events=[json.loads(x) for x in stages.read_text(encoding='utf-8').splitlines()]
        facts.append(fact.read_bytes());artifacts.append(digest(artifact));rows[mode]={'timing':timing,'counts':count,'stages':events,'fact_nodes':factdata['node_count'],'builtin_symbols':factdata['builtin_symbols'],'facts_sha256':digest(fact)}
        for i in range(6):assert sum(v[i] for v in count['copies'].values())==sum(sum(v[i] for v in phase.values()) for phase in count['phases'])
        if mode=='disabled':assert not count['passes'] and all(sum(v)==0 for v in count['copies'].values())
        else:
            assert count['passes'] and len(count['passes'])<=32
            for p in count['passes']:
                fns=[r for r in count['functions'] if r['summary'] and r['pass']==p['pass']];assert len(fns)==p['functions'];assert sum(r['return_changed'] for r in fns)==p['changed_returns'] and sum(r['effect_changed'] for r in fns)==p['changed_effects']
            assert not count['passes'][-1]['changed'] or count['summary_fallbacks']==1
            for function in count['functions']:
                if not function['summary']:assert function['pass']==0
                for name,version,n in function['reads']:assert version>=0 and n>0
    if disabled:assert facts[0]==facts[1]
    result={'status':'passed','case':c,'rows':rows,'artifact_sha256':digest(normal),'disabled_enabled_facts_equal':disabled,'same_generated_huab':True}
    save(destination,result);print(json.dumps({'case':c['name'],'normal_seconds':rows['normal']['seconds'],'counted_seconds':rows['enabled']['timing']['seconds'],'passes':len(rows['enabled']['counts']['passes']),'status':'passed'}),flush=True)

def host(root,out):
    verify(root,out);small=json.loads((out/'parser-closure/result.json').read_text(encoding='utf-8'));sources=closure(root/'selfhost/archive_main.hua');total=sum(p.stat().st_size for p in sources.values());ratio=total/small['case']['bytes'];estimate=small['rows']['enabled']['timing']['seconds']*ratio*ratio*3
    assert estimate<90,'first revise the bounded diagnostic plan; do not raise the deadline blindly'
    case={'name':'host-14-modules','path':str(root/'selfhost/archive_main.hua'),'sha256':digest(root/'selfhost/archive_main.hua'),'bytes':total,'kind':'host-only','modules':{k:digest(v) for k,v in sources.items()}}
    save(out/'host-plan.json',{'case':case,'limit_seconds':90,'heuristic_supervision_estimate_seconds':estimate,'estimate_scope':'Source-ratio squared with factor three is solely a diagnostic watchdog heuristic, not a complexity proof or BO-G4 prediction. Existing prior host preparation was 37s on another machine.','native_host_only':True,'hua_vm_own_source':False})
    run_case(root,out,case,disabled=True,limit=90)
    assert {k:digest(v) for k,v in sources.items()}==case['modules']

def historical(root,out):
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'))
    frozen={'native':out/'baseline-runtime.exe','probe':out/'baseline-tools/probe.exe','guarded-cli':out/'baseline-tools/guarded-cli.exe'}
    for key,t in m['tools'].items():assert digest(frozen[key])==t['sha256'],key
    for c in m['inputs']:assert digest(c['path'])==c['sha256'],c
    return m

def compare(root,out):
    m=historical(root,out);cases=m['inputs']+[json.loads((out/'host-plan.json').read_text(encoding='utf-8'))['case']];target=out/'candidate';target.mkdir();rows=[]
    tools={'native':root/'build/hua.exe','probe':root/'build-bo08-sema-20261016/hua_sema_scale_probe.exe'};identities={k:{'path':str(v),'sha256':digest(v)} for k,v in tools.items()};save(target/'tools.json',identities)
    for c in cases:
        prior=out/c['name'];old=json.loads((prior/'result.json').read_text(encoding='utf-8'));assert old['status']=='passed';caseout=target/c['name'];caseout.mkdir();artifact=caseout/'normal.huab';counted=caseout/'enabled.huab';count=caseout/'counts.json';facts=caseout/'facts.json';stages=caseout/'stages.jsonl';limit=90 if c['name']=='host-14-modules' else 30
        normal,_,_=command(root,out,'candidate-'+c['name']+'-native',[tools['native'],'build',c['path'],'-o',artifact],limit=limit)
        observed,_,_=command(root,out,'candidate-'+c['name']+'-counts',[tools['probe'],count,stages,facts,c['path'],counted,'enabled'],limit=limit)
        assert artifact.read_bytes()==counted.read_bytes()==(prior/'normal.huab').read_bytes(),(c['name'],'artifact differs')
        assert facts.read_bytes()==(prior/'enabled-facts.json').read_bytes(),(c['name'],'facts differ')
        before=old['rows']['enabled']['counts'];after=json.loads(count.read_text(encoding='utf-8'));eager=before['copies'].get('binary_eager',[0]*6)
        assert after['area_calls'][1]==0 and all(v==0 for v in after['copies'].get('binary_eager',[0]*6))
        assert before['copies'].get('binary_logic',[0]*6)==after['copies'].get('binary_logic',[0]*6)
        assert before['passes']==after['passes'] and before['summary_fallbacks']==after['summary_fallbacks']
        # Copy construction versus assignment may differ in container node reuse;
        # total events per semantic type must fall by exactly the removed snapshot.
        for i in (0,2,4):
            expected=sum(v[i]+v[i+1] for v in before['copies'].values())-eager[i]-eager[i+1]
            actual=sum(v[i]+v[i+1] for v in after['copies'].values());assert actual==expected,(c['name'],i,actual,expected)
        normalize=lambda fs:[(f['name'],f['pass'],f['summary'],f['return_changed'],f['effect_changed'],f['reads']) for f in fs]
        assert normalize(before['functions'])==normalize(after['functions']),(c['name'],'summary/dependency observation differs')
        for key in ('joins','alias_scans','alias_symbols','alias_views','exits','loop_passes','field_requests','field_hits','field_builds'):assert before[key]==after[key],(c['name'],key)
        row={'case':c,'before_normal':old['rows']['normal'],'after_normal':normal,'after_diagnostic':observed,'removed_copies':eager,'after_counts':after,'same_huab_and_facts':True,'same_summaries_dependencies':True}
        save(caseout/'result.json',{'status':'passed',**row});rows.append(row);save(target/'results.json',{'status':'running','rows':rows});print(json.dumps({'case':c['name'],'before':old['rows']['normal']['seconds'],'after':normal['seconds'],'eager_symbol_copies_removed':eager[4]+eager[5]}),flush=True)
    for key,t in identities.items():assert digest(t['path'])==t['sha256']
    save(target/'results.json',{'status':'passed','cases':len(rows),'rows':rows,'no_baseline_workload_rerun':True,'full_own_source_run':False})

def analyze(root,out):
    m=historical(root,out);candidate=json.loads((out/'candidate/results.json').read_text(encoding='utf-8'));assert candidate['status']=='passed' and candidate['cases']==21
    oldrows={c['name']:json.loads((out/c['name']/'result.json').read_text(encoding='utf-8')) for c in m['inputs']};oldrows['host-14-modules']=json.loads((out/'host-14-modules/result.json').read_text(encoding='utf-8'))
    checks=0;models=[]
    for name,r in oldrows.items():
        c=r['case'];count=r['rows']['enabled']['counts'];builtin=r['rows']['enabled']['builtin_symbols'];kind=c['kind']
        if kind in ('scalar','logic'):
            logic=kind=='logic';area='binary_logic' if logic else 'binary_eager';v=count['copies'][area];copies=3*c['b']*(builtin+c['f']+c['n']+logic)
            assert len(count['passes'])==2;assert count['area_calls'][2 if logic else 1]==3*c['b'];assert v[4]+v[5]==copies;assert v[0]+v[1]==0;checks+=4
            models.append({'case':name,'formula':'3*B*(builtin symbols+functions+locals+boolean parameter)','predicted_symbol_copies':copies})
        elif kind=='functions':
            f=c['f'];g=builtin+f;v=count['copies']['binary_eager'];expected=3*(f-1)*g
            assert len(count['passes'])==2;assert count['area_calls'][1]==3*(f-1);assert v[4]+v[5]==expected;assert v[0]+v[1]==0;assert sum(count['copies']['function_reset'][4:6])==3*f*g;checks+=5
            models.append({'case':name,'formula':'3*(F-1)*(builtin symbols+F) eager; 3*F*(builtin symbols+F) reset','predicted_eager_symbols':expected,'predicted_reset_symbols':3*f*g})
        elif kind=='views':
            w=c['w'];v=count['copies']['binary_eager'];assert v[0]+v[1]==144*w;assert v[2]+v[3]==48*w;assert v[4]+v[5]==12*(builtin+2+2+4);assert count['area_calls'][1]==12;checks+=4
            models.append({'case':name,'formula':'12 snapshots; each has four locals, each local has 3*W Views entries; origins have W entries per local','predicted_info_copies':144*w,'predicted_origin_copies':48*w})
    save(out/'work-model.json',{'status':'passed','checks':checks,'rows':models,'scope':'exact copy events for stated bounded fixture shapes, not allocation/byte/CPU totals'})
    baseline=oldrows['host-14-modules'];b=baseline['rows']['enabled']['counts'];after=next(r for r in candidate['rows'] if r['case']['name']=='host-14-modules');a=after['after_counts']
    totals=lambda c:[sum(v[i]+v[i+1] for v in c['copies'].values()) for i in (0,2,4)]
    visits=[r for r in b['functions'] if r['summary']];unchanged=[r for r in visits if not r['return_changed'] and not r['effect_changed']];previous={};same_read_versions=0
    for row in visits:
        old=previous.get(row['name'])
        if old and old['reads']==row['reads'] and not row['return_changed'] and not row['effect_changed']:same_read_versions+=1
        previous[row['name']]=row
    facts=json.loads((out/'host-14-modules/enabled-facts.json').read_text(encoding='utf-8'));functions=sum(n[0]==5 for n in facts['nodes']);structures=sum(n[0]==8 for n in facts['nodes']);containers=[n for n in facts['nodes'] if n[0] in (1,5)];globals_=[n for n in facts['nodes'] if n[0] in (2,3,4) and not any(p[2]==n[2] and p[3]<=n[3]<p[4] for p in containers)];g=facts['builtin_symbols']+functions+structures+len(globals_)
    assert sum(b['copies']['function_reset'][4:6])==g*len(b['functions']);checks_reset=True
    commands=[json.loads(p.read_text(encoding='utf-8')) for p in (out/'commands').glob('*.json')];failures=[r for r in commands if r['returncode']!=0 or r['timeout']]
    assert len(failures)==1 and failures[0]['label']=='build-sema-profile' and not any(r['timeout'] for r in commands)
    tests=(out/'commands/ctest-eager-candidate.stdout').read_text(encoding='utf-8');assert '100% tests passed out of 7' in tests
    unit=(out/'commands/sema-profile-unit.stdout').read_text(encoding='utf-8');assert unit.startswith('19 ')
    registration=(out/'commands/ctest-profile-registration.stdout').read_text(encoding='utf-8');assert 'Total Tests: 1' in registration
    assert json.loads((out/'off-build-after.json').read_text(encoding='utf-8'))['same_pe_text']
    api=json.loads((root/'build/a07-completion/results.json').read_text(encoding='utf-8'));assert api['status']=='passed' and api['cases']==119 and api['checks']==1777
    rows=[]
    for entry in candidate['rows']:
        name=entry['case']['name'];old=oldrows[name];rows.append({'case':name,'before_seconds':entry['before_normal']['seconds'],'after_seconds':entry['after_normal']['seconds'],'before_cpu':entry['before_normal']['cpu_seconds'],'after_cpu':entry['after_normal']['cpu_seconds'],'copies_removed':entry['removed_copies'],'facts_nodes':old['rows']['enabled']['fact_nodes']})
    result={'status':'passed','baseline_git':json.loads((out/'plan.json').read_text(encoding='utf-8'))['git_head'],'cases':21,'work_model_checks':checks,'global_reset_model_checked':checks_reset,'native_unit_checks':19,'native_contract':{'cases':119,'checks':1777},'candidate_ctest_tests':7,'ctest_outer_seconds':next(r['seconds'] for r in commands if r['label']=='ctest-eager-candidate'),'host_14':{'source_bytes':276909,'modules':14,'analyzed_functions':b['passes'][0]['functions'],'ast_nodes':facts['node_count'],'passes':b['passes'],'fallbacks':b['summary_fallbacks'],'summary_visits':len(visits),'unchanged_visits':len(unchanged),'same_recorded_reads_and_unchanged':same_read_versions,'copy_event_order':['Info including Views entries','Origin records','Symbol records'],'copy_totals_before':totals(b),'copy_totals_after':totals(a),'binary_snapshots_before':{'eager':b['area_calls'][1],'logical':b['area_calls'][2]},'binary_snapshots_after':{'eager':a['area_calls'][1],'logical':a['area_calls'][2]},'alias_scans':b['alias_scans'],'alias_symbols_scanned':b['alias_symbols'],'alias_views_scanned':b['alias_views'],'field_requests':b['field_requests'],'field_hits':b['field_hits'],'field_builds':b['field_builds'],'global_symbols':g,'function_resets':len(b['functions'])},'rows':rows,'candidate_tools':json.loads((out/'candidate/tools.json').read_text(encoding='utf-8')),'command_count':len(commands),'logged_command_wall_sum':sum(r['seconds'] for r in commands),'timeouts':0,'retained_build_failure':failures,'full_own_source_run':False,'hua_checker_source_changed':False,'limits_unchanged':True,'observability_limits':['Copy events are nested and not disjoint allocations/bytes/CPU.','max_views/max_origins sample fields() prototype/roots, not all live environment maxima.','Recorded direct-call return/effect versions do not establish complete closure/global/indirect-call invalidation.','Function durations identify the Hua source function being analyzed by the C++ host, not the C++ stack function CPU share.','Single before/after ordinary timings are local observations, not stable general speedup or Hua VM own-compilation prediction.'],'next':'Measure existing Hua capabilities dirty/dependents hit/revisit and copy/equality cost with small inputs; binary conditional snapshots and dirty scheduling already exist there. Do not duplicate them.'}
    save(out/'summary.json',result);print(json.dumps({k:v for k,v in result.items() if k not in ('rows','retained_build_failure')},ensure_ascii=False,indent=2))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('init','calibrate','matrix','real','host','compare','analyze'));p.add_argument('--output',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1];out=a.output.resolve();assert out.is_relative_to(root/'build') and out!=root/'build';os.environ['PATH']=str(root/'build')+os.pathsep+os.environ.get('PATH','')
    if a.mode=='init':initialize(root,out);return
    if a.mode=='analyze':analyze(root,out);return
    if a.mode=='compare':compare(root,out);return
    if a.mode=='host':host(root,out);return
    m=verify(root,out)
    selected=[c for c in m['inputs'] if (c['name'].startswith('calibration') if a.mode=='calibrate' else (c['kind'] in ('real','closure') if a.mode=='real' else not c['name'].startswith('calibration') and c['kind'] not in ('real','closure')))]
    for c in selected:run_case(root,out,c,disabled=a.mode=='calibrate' or c['kind']=='closure')
if __name__=='__main__':main()
