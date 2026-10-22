from pathlib import Path
import argparse, json, os, shutil, subprocess
from audit_bootstrap_phases import command,digest,save,closure,reduce_events

def initialize(root,out):
    assert (out/'plan.json').exists() and not (out/'manifest.json').exists()
    prior=root/'tests/artifacts/build/bo03-20261016'; component=prior/'queries/compiler.huab'
    sources=closure(root/'selfhost/archive_main.hua')
    for name,path in sources.items():assert path.read_bytes()==(prior/'queries-source'/name).read_bytes(),name
    inputs={name:prior/'inputs'/(name+'.hua') for name in ('catalog','type_rules')}
    tools={}; target=out/'before';target.mkdir()
    for kind,source in {'native':root/'tests/artifacts/build/hua.exe','probe':root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe'}.items():
        assert source.stat().st_mtime>(out/'plan.json').stat().st_mtime, 'build must finish: '+str(source)
        destination=target/(kind+'.exe');shutil.copy2(source,destination);tools[kind]={'path':str(destination),'sha256':digest(destination)}
    shutil.copy2(root/'tests/artifacts/build/libhua_semantics.a',target/'libhua_semantics.a');shutil.copy2(root/'tests/artifacts/build-a11-profile/libhua_semantics.a',target/'libhua_semantics-profile.a')
    shutil.copy2(component,out/'compiler.huab')
    save(out/'manifest.json',{'tools':tools,'component_sha256':digest(component),'source_hashes':{k:digest(p) for k,p in sources.items()},'inputs':{k:{'path':str(v),'sha256':digest(v)} for k,v in inputs.items()},'host_git':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'prior_component':'BO-03 selected Parser source matches all 14 modules; reused HUAB, no C0 rebuild','full_own_source_run':False})

def real(root,out,tag):
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'));target=out/tag;target.mkdir(exist_ok=True);assert not (target/'real.json').exists()
    tools=m['tools'] if tag=='before' else {k:{'path':str(root/p),'sha256':digest(root/p)} for k,p in {'native':'tests/artifacts/build/hua.exe','probe':'tests/artifacts/build-a11-profile/hua_performance_probe.exe'}.items()}
    save(target/'tools.json',tools);rows=[]
    for name,source in m['inputs'].items():
        path=Path(source['path']);assert digest(path)==source['sha256'] and digest(out/'compiler.huab')==m['component_sha256']
        a=target/(name+'.huab');b=target/(name+'-counted.huab');events=target/(name+'-events.jsonl')
        normal,report,stderr=command(root,out,tag+'-'+name+'-normal',[tools['native']['path'],'run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',path,a])
        diagnostic,diagout,diagerr=command(root,out,tag+'-'+name+'-counts',[tools['probe']['path'],'--phase-events',events,out/'compiler.huab',path,b])
        assert report==diagout and a.read_bytes()==b.read_bytes()
        prior=root/'tests/artifacts/build/bo03-20261016/queries'/name
        assert a.read_bytes()==prior.with_suffix('.huab').read_bytes(),name
        trace=[json.loads(line) for line in events.read_text(encoding='utf-8').splitlines()];counts=trace[-1]['counters'];budget=json.loads(stderr)
        assert counts==json.loads(diagerr.decode().splitlines()[0]);metrics=json.loads(diagerr.decode().splitlines()[-1]);assert budget['steps']==metrics['steps'] and budget['peak_calls']==metrics['peak_calls']
        assert sum(counts['result_origins'].values())==counts['result_constraints']
        row={'case':name,'normal':normal,'diagnostic':diagnostic,'counts':counts,'budget':budget,'report_sha256':__import__('hashlib').sha256(report).hexdigest(),'artifact_sha256':digest(a)}
        if tag!='before':
            old=next(r for r in json.loads((out/'before/real.json').read_text(encoding='utf-8'))['rows'] if r['case']==name)
            assert row['report_sha256']==old['report_sha256'] and row['artifact_sha256']==old['artifact_sha256'] and budget==old['budget']
            for key in ('vm_steps','user_calls','enforce_type_calls','result_constraints','result_ok_constraints','result_err_constraints','result_void_constraints','result_payload_type_changes','result_materializations','result_origins'):assert counts[key]==old['counts'][key],(name,key)
        rows.append(row);save(target/'real.json',{'status':'running','rows':rows})
    save(target/'real.json',{'status':'passed','rows':rows,'full_own_source_run':False})
    print(json.dumps([{'case':r['case'],'normal_cpu':r['normal']['cpu_seconds'],**{k:v for k,v in r['counts'].items() if k.startswith('result_')}} for r in rows],indent=2))

def kernels(root,out):
    from audit_value_calls import cases,source,parse_output,operations
    target=out/'kernels';target.mkdir(exist_ok=True);assert not (target/'results.json').exists();rows=[]
    selected=[c for c in cases() if c['id'] in {'struct-w8-d1-r0-n512','struct-w8-d1-r1-n512','struct-w8-d1-r2-n512','struct-w8-d1-r4-n512','struct-w5-d3-r3-n768','struct-w16-d2-r2-n768'}]
    save(target/'plan.json',{'cases':selected,'rounds':'fixed, no adaptive candidate changes','source_vm_ast_source_free_huab':True,'old_and_new_huab_equal':True})
    clang=root/'.tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe';wasm=root/'.tools/wasmtime-v49.0.2-x86_64-windows-c-api/lib/wasmtime.dll.lib'
    for kind,profile in [('native',False),('count',True)]:
        args=[clang,'-std=c++20','-O3','-DNDEBUG','-Iinclude']+(['-DHUA_INTERNAL_PROFILE=1'] if profile else [])+['tests/value_call_kernel_probe.cpp',root/('tests/artifacts/build-a11-profile/libhua_semantics.a' if profile else 'tests/artifacts/build/libhua_semantics.a'),root/'tests/artifacts/build/libhua_frontend.a','-o',out/'after'/(kind+'-kernel.exe'),wasm,'-static','-lpthread','-Wl,--stack,8388608']
        command(root,out,'build-after-'+kind+'-kernel',args,limit=90)
    for case in selected:
        stem=case['id'];path=target/(stem+'.hua');path.write_text(source(case),encoding='utf-8',newline='\n');artifact=target/(stem+'.huab');other=target/(stem+'-after.huab');pair={}
        for tag in ('before','after'):
            native=out/'before/native.exe' if tag=='before' else root/'tests/artifacts/build/hua.exe'
            probe=out/'before/probe.exe' if tag=='before' else root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe'
            native_ops={}
            for kind in ('native','count'):
                _,stdout,_=command(root,out,tag+'-'+stem+'-'+kind,[out/tag/(kind+'-kernel.exe'),case['kind'],case['width'],case['depth'],case['results'],case['rounds']])
                native_ops[kind]=[json.loads(line) for line in stdout.decode().splitlines()]
                for row in native_ops[kind]:assert row['checksum']==17*row['operations']
            for mode in ('interpret','run','build'):
                _,stdout,_=command(root,out,tag+'-'+stem+'-'+mode,[native,mode,path]+(['-o',artifact if tag=='before' else other] if mode=='build' else []))
                if mode!='build':parse_output(stdout,case)
            if tag=='after':assert artifact.read_bytes()==other.read_bytes()
            hidden=path.with_suffix('.hidden');path.rename(hidden)
            try:
                _,stdout,_=command(root,out,tag+'-'+stem+'-huab',[native,'run',artifact]);parsed=parse_output(stdout,case)
                events=target/(stem+'-'+tag+'-events.jsonl');_,stdout,stderr=command(root,out,tag+'-'+stem+'-counts-with-argument',[probe,'--phase-events',events,artifact,'result-kernel']);counted=parse_output(stdout,case)
                for op in parsed:assert parsed[op]['managed_allocations']==counted[op]['managed_allocations']
                trace=[json.loads(line) for line in events.read_text(encoding='utf-8').splitlines()];phases=reduce_events(trace)
            finally:hidden.rename(path)
            pair[tag]={'native':native_ops,'hua':{op:{'normal':parsed[op],'counts':phases[op]['counters']} for op in operations(case)}}
        r=case['results'];d=case['depth'];w=case['width'];n=case['rounds']
        native_struct={'read':0,'copy':d,'move-shared':d,'move-unique':0,'enforce':d if r else 0,'borrow':d if r else 0,'argument':d,'return':d,'roundtrip':2*d}
        native_result={'read':0,'copy':r,'move-shared':r,'move-unique':0,'enforce':0,'borrow':0,'argument':r,'return':r,'roundtrip':2*r}
        hua_struct={'read':0,'copy':d,'argument':d,'return':d,'roundtrip':2*d,'borrow':0}
        hua_result={'read':0,'copy':0,'argument':r,'return':r,'roundtrip':2*r,'borrow':0}
        for row in pair['after']['native']['count']:
            op=row['operation'];c=row['counts']
            if op not in native_struct:continue
            assert c['copy_struct']==n*native_struct[op],(stem,op,'native Struct',c['copy_struct'])
            assert c['copy_result']==n*native_result[op],(stem,op,'native Result',c['copy_result'])
            assert c['copy_struct_fields']==w*c['copy_struct']
        for op,new in pair['after']['hua'].items():
            old=pair['before']['hua'][op];c=new['counts']
            assert c['copy_struct']==n*hua_struct[op],(stem,op,'Hua Struct',c['copy_struct'])
            assert c['copy_result']==n*hua_result[op],(stem,op,'Hua Result',c['copy_result'])
            assert c['copy_struct_fields']==w*c['copy_struct']
            for key in ('vm_steps','user_calls','enforce_type_calls','result_constraints','result_materializations','result_payload_type_changes'):assert c[key]==old['counts'][key],(stem,op,key)
        rows.append({'case':case,'pair':pair,'artifact_sha256':digest(artifact)});save(target/'results.json',{'status':'running','rows':rows})
    save(target/'results.json',{'status':'passed','rows':rows,'cases':len(rows),'hua_operations_per_version':sum(len(r['pair']['after']['hua']) for r in rows),'native_operations_per_build':sum(len(r['pair']['after']['native']['count']) for r in rows)})

def paired(root,out):
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'));target=out/'paired';target.mkdir();rows=[]
    save(target/'plan.json',{'rounds':3,'calls':12,'inputs':['catalog','type_rules'],'rotating_order':[['before','after'],['after','before'],['before','after']],'no_more_candidate_changes':True,'normal_build_only':True})
    for round_index,order in enumerate([('before','after'),('after','before'),('before','after')]):
        for name,source in m['inputs'].items():
            for tag in order:
                native=out/'before/native.exe' if tag=='before' else root/'tests/artifacts/build/hua.exe';artifact=target/f'{round_index}-{name}-{tag}.huab'
                timing,stdout,stderr=command(root,out,f'paired-{round_index}-{name}-{tag}',[native,'run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',source['path'],artifact])
                old=next(r for r in json.loads((out/'before/real.json').read_text(encoding='utf-8'))['rows'] if r['case']==name)
                assert artifact.read_bytes()==(out/'before'/(name+'.huab')).read_bytes() and __import__('hashlib').sha256(stdout).hexdigest()==old['report_sha256'] and json.loads(stderr)==old['budget']
                rows.append({'round':round_index,'case':name,'tag':tag,'timing':timing});save(target/'results.json',{'status':'running','rows':rows})
    import statistics
    summary={name:{tag:{metric:statistics.median(r['timing'][metric] for r in rows if r['case']==name and r['tag']==tag) for metric in ('seconds','cpu_seconds')} for tag in ('before','after')} for name in m['inputs']}
    save(target/'results.json',{'status':'passed','rows':rows,'medians':summary});print(json.dumps(summary,indent=2))

def analyze(root,out):
    import statistics
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'));before=json.loads((out/'before/real.json').read_text(encoding='utf-8'));after=json.loads((out/'after/real.json').read_text(encoding='utf-8'));kernels=json.loads((out/'kernels/results.json').read_text(encoding='utf-8'));paired=json.loads((out/'paired/results.json').read_text(encoding='utf-8'))
    for result in (before,after,kernels,paired):assert result['status']=='passed'
    checks=0;model=[]
    for row in kernels['rows']:
        case=row['case'];d=case['depth'];r=case['results'];w=case['width'];n=case['rounds']
        for tag in ('before','after'):
            old=tag=='before';native=row['pair'][tag]['native']['count']
            ns={'read':0,'copy':d,'move-shared':d,'move-unique':0,'enforce':d*r if old else (d if r else 0),'borrow':d*r if old else (d if r else 0),'argument':d*(r+1) if old else d,'return':d*(r+1) if old else d,'roundtrip':2*d*(r+1) if old else 2*d}
            nr={'read':0,'copy':r,'move-shared':r,'move-unique':0,'enforce':r*(r-1)//2 if old else 0,'borrow':r*(r-1)//2 if old else 0,'argument':r*(r+1)//2 if old else r,'return':r*(r+1)//2 if old else r,'roundtrip':r*(r+1) if old else 2*r}
            na={'read':0,'copy':d+r,'move-shared':d+r,'move-unique':0,'enforce':d*r+r*(r+1)//2 if old else (d+r if r else 0),'borrow':d*r+r*(r+1)//2 if old else (d+r if r else 0),'argument':d*(r+1)+r*(r+3)//2 if old else d+2*r,'return':d*(r+1)+r*(r+3)//2 if old else d+2*r,'roundtrip':2*d*(r+1)+r*(r+3) if old else 2*d+4*r}
            for entry in native:
                op=entry['operation']
                if op not in ns:continue
                c=entry['counts']
                for key,factor in [('copy_struct',ns[op]),('copy_result',nr[op]),('copy_struct_fields',w*ns[op]),('managed_allocations',na[op])]:assert c[key]==n*factor,(case,tag,op,key,c[key],n*factor);checks+=1
            hs={'read':0,'copy':d*max(1,r) if old else d,'argument':d*(r+1) if old else d,'return':d*(2*r+1) if old else d,'roundtrip':d*(3*r+2) if old else 2*d,'borrow':0}
            hr={'read':0,'copy':r*(r-1)//2 if old else 0,'argument':r*(r+1)//2 if old else r,'return':r*r if old else r,'roundtrip':r*(3*r+1)//2 if old else 2*r,'borrow':0}
            for op,entry in row['pair'][tag]['hua'].items():
                c=entry['counts']
                for key,factor in [('copy_struct',hs[op]),('copy_result',hr[op]),('copy_struct_fields',w*hs[op])]:assert c[key]==n*factor,(case,tag,op,key);checks+=1
        model.append({'case':case,'old_roundtrip_structs':d*(3*r+2),'new_roundtrip_structs':2*d,'old_roundtrip_result_copies':r*(3*r+1)//2,'new_roundtrip_result_copies':2*r})
    real=[]
    for a,b in zip(before['rows'],after['rows']):
        assert a['case']==b['case'];c=a['counts'];v=b['counts']
        assert v['result_materializations']==c['result_materializations'] and v['result_constraints']==c['result_constraints']
        assert v['copy_struct']<c['copy_struct'] and v['copy_struct_fields']<c['copy_struct_fields'] and v['managed_allocations']<c['managed_allocations']
        timing=paired['medians'][a['case']]
        real.append({'case':a['case'],'before_counts':c,'after_counts':v,'paired_medians':timing,'paired_wall_reduction_percent':100*(1-timing['after']['seconds']/timing['before']['seconds']), 'timing_ranges':{tag:{metric:[min(x['timing'][metric] for x in paired['rows'] if x['case']==a['case'] and x['tag']==tag),max(x['timing'][metric] for x in paired['rows'] if x['case']==a['case'] and x['tag']==tag)] for metric in ('seconds','cpu_seconds')} for tag in ('before','after')}})
    commands=[json.loads(path.read_text(encoding='utf-8')) for path in (out/'commands').glob('*.json')];failed=[x for x in commands if x['timeout'] or x['returncode']!=0]
    assert all(not x['timeout'] for x in commands)
    assert len(failed)==1 and failed[0]['label']=='before-struct-w8-d1-r0-n512-counts' and failed[0]['returncode']==2
    ctest=next(x for x in commands if x['label']=='ctest-result-neighbors');assert ctest['returncode']==0
    text=(out/'commands/ctest-result-neighbors.stdout').read_text(encoding='utf-8');assert '100% tests passed out of 14' in text
    save(out/'work-model.json',{'status':'passed','model_checks':checks,'scope':'acyclic single-chain Struct with R nested Results, measured operations only; not CPU or whole compiler growth prediction','rows':model})
    summary={'status':'passed','git_baseline':m['host_git'],'native_before':m['tools']['native'],'native_after':{'path':str(root/'tests/artifacts/build/hua.exe'),'sha256':digest(root/'tests/artifacts/build/hua.exe')},'probe_after_sha256':digest(root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe'),'component_sha256':m['component_sha256'],'compiler_modules':len(m['source_hashes']),'compiler_sources':m['source_hashes'],'model_checks':checks,'kernel_cases':kernels['cases'],'hua_operations_per_version':kernels['hua_operations_per_version'],'native_operations_per_build':kernels['native_operations_per_build'],'native_contract_checks_per_version':56,'ctest_tests':14,'ctest_seconds':ctest['seconds'],'real':real,'command_count':len(commands),'logged_command_seconds_sum':sum(x['seconds'] for x in commands),'timeouts':0,'retained_harness_failure':failed,'own_source_accepted':False,'full_own_source_run':False,'limits_unchanged':True,'timing_conclusion':'Three fixed paired rounds: small wall changes with overlapping ranges; CPU medians each differ by one Windows accounting quantum. No reliable overall speedup claim.'}
    save(out/'summary.json',summary)
    print(json.dumps({k:v for k,v in summary.items() if k not in ('real','compiler_sources','retained_harness_failure')},indent=2))

def main():
    p=argparse.ArgumentParser(description='BO-04 Result materialization, bounded same-artifact diagnostics. Uses retained BO-03 source-matched artifacts.');p.add_argument('mode',choices=('init','real','kernels','paired','analyze'));p.add_argument('--tag',choices=('before','after'),default='before');p.add_argument('--resume',action='store_true');p.add_argument('--output',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();assert out.is_relative_to(root/'tests/artifacts') and out!=root/'tests/artifacts';os.environ['PATH']=str(root/'tests/artifacts/build')+os.pathsep+os.environ.get('PATH','')
    if a.resume:
        assert a.mode=='kernels'
        import audit_bootstrap_phases as audit
        manifest=json.loads((out/'manifest.json').read_text(encoding='utf-8'));tools=json.loads((out/'after/tools.json').read_text(encoding='utf-8'))
        for item in list(manifest['tools'].values())+list(tools.values()):assert digest(item['path'])==item['sha256']
        plan=json.loads((out/'plan.json').read_text(encoding='utf-8'));assert digest(root/'tests/value_call_kernel_probe.cpp')==plan['files']['tests/value_call_kernel_probe.cpp']
        ledger=[]
        for path in sorted((out/'commands').glob('*.json')):
            row=json.loads(path.read_text(encoding='utf-8'))
            if row['returncode']==0 and not row['timeout']:ledger.append({'label':row['label'],'metadata_sha256':digest(path),'stdout_sha256':digest(path.with_suffix('.stdout')),'stderr_sha256':digest(path.with_suffix('.stderr'))})
        save(out/'kernels-resume-ledger.json',{'reason':'Missing diagnostic program argument rejected before executing kernel; preserve original failure, add one placeholder arg, reuse reviewed successful commands','native_and_probe_identities_verified':True,'kernel_source_verified':True,'completed':ledger})
        audit.RESUME={'completed_labels':{r['label'] for r in ledger}}
    if a.mode=='init':initialize(root,out)
    elif a.mode=='real':real(root,out,a.tag)
    else:{'kernels':kernels,'paired':paired,'analyze':analyze}[a.mode](root,out)
if __name__=='__main__':main()
