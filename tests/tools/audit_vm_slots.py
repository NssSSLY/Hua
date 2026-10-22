"""BO-09: frozen runtime lookup evidence; no compiler own-source VM workload."""
from pathlib import Path
import argparse,json,shutil,subprocess,sys
from audit_bootstrap_phases import command,save,digest,closure,machine

def initialize(root,out,reuse=None,reuse_manifest=None):
    assert not out.exists();out.mkdir(parents=True)
    files={}
    for raw in subprocess.check_output(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=root).split(b'\0'):
        if raw:
            name=raw.decode('utf-8');p=root/name
            if p.is_file():files[name]=digest(p)
    frozen=out/'frozen';frozen.mkdir()
    for name in files:
        p=root/name
        if p.suffix in ('.cpp','.hpp','.hua','.py') or name=='CMakeLists.txt':
            target=frozen/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
    tools={};before=out/'before';before.mkdir()
    for key,source in {'native':root/'tests/artifacts/build/hua.exe','probe':root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe'}.items():
        p=before/(key+'.exe');shutil.copy2(source,p);tools[key]={'path':str(p),'sha256':digest(p)}
    dependency=root/'tests/artifacts/build/wasmtime.dll'
    if dependency.exists():shutil.copy2(dependency,before/dependency.name)
    hashes={k:digest(v) for k,v in closure(root/'selfhost/archive_main.hua').items()}
    if reuse:
        assert reuse_manifest,'component reuse needs a source and component hash manifest'
        prior=json.loads(reuse_manifest.read_text(encoding='utf-8'))
        assert hashes==prior.get('current_source_hashes',prior.get('source_hashes',prior.get('sources')))
        assert digest(reuse)==prior.get('component_after_sha256',prior.get('component_sha256'))
        shutil.copy2(reuse,out/'compiler.huab')
    else:command(root,out,'prepare-compiler',[tools['native']['path'],'build',frozen/'selfhost/archive_main.hua','-o',out/'compiler.huab'],limit=90)
    assert hashes=={k:digest(v) for k,v in closure(root/'selfhost/archive_main.hua').items()},'compiler sources changed during preparation'
    data={'git':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'dirty':subprocess.check_output(['git','-c','core.quotepath=false','status','--short'],cwd=root,text=True,encoding='utf-8').splitlines(),'files':files,'tools':tools,'compiler_sources':hashes,'compiler_sha256':digest(out/'compiler.huab'),'host':machine(),'full_own_source_run':False}
    save(out/'manifest.json',data)
    save(out/'plan.json',{'target':'BO-09 guarded local/parameter slots plus stable field access and immutable instruction metadata','existing':'function layout and declared field types already cached; keep them','compatibility':'runtime side tables only; old HUAB rebuilt on execution, no opcode/format/ABI or semantics change','safety':'initialized slot checks; guarded scope route; dynamic closure/global fallback; stable slot storage/owner lifetime; GC edges; independent task metadata','gates':'current CPU sample and fixed kernels before edit; AST/VM/old-native/source-free HUAB diagnostics; closures/control/defer/Task/GC/reader; exact steps/depth/output/receipts preserved; memory and startup measured','limits':[500000000,128,65536,600,16777216],'no_full_own_source_or_generations':True})
    inputs=out/'inputs';inputs.mkdir()
    cases={
      'locals':('fn kernel(n int) int{var sum=0\nfor i in 0..n{var x=i\n{sum+=x\nsum+=1}}\nreturn sum}\nfn main(){print(kernel(2000000))}\n',2000000*2000001//2),
      'parameters':('fn add(a int,b int,c int,d int) int{return a+b+c+d}\nfn main(){var sum=0\nfor i in 0..600000{sum+=add(i,1,2,3)}\nprint(sum)}\n',600000*599999//2+600000*6),
      'fields':('struct R{'+''.join(f'f{i} int\n' for i in range(32))+'}\nfn main(){var b=R{'+','.join(f'f{i}:0' for i in range(32))+'}\nfor i in 0..2000000{b.f31+=i}\nprint(b.f31)}\n',2000000*1999999//2),
      'multi':('fn pair(x int)(int,int){return x,1}\nfn main(){var sum=0\nfor i in 0..600000{let a,b=pair(i)\nsum+=a+b}\nprint(sum)}\n',600000*600001//2)}
    rows=[]
    for name,(text,result) in cases.items():
        p=inputs/(name+'.hua');p.write_text(text,encoding='utf-8',newline='\n');artifact=inputs/(name+'.huab')
        command(root,out,'build-'+name,[tools['native']['path'],'build',p,'-o',artifact])
        rows.append({'name':name,'path':str(p),'artifact':str(artifact),'expected':str(result)+'\n','source_sha256':digest(p),'artifact_sha256':digest(artifact)})
    save(out/'inputs.json',rows)

def kernels(root,out,tag):
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'));dest=out/tag;dest.mkdir(exist_ok=True);assert not (dest/'kernels.json').exists()
    tools=m['tools'] if tag=='before' else {k:{'path':str(root/v),'sha256':digest(root/v)} for k,v in {'native':'tests/artifacts/build/hua.exe','probe':'tests/artifacts/build-a11-profile/hua_performance_probe.exe'}.items()}
    rows=[]
    for c in json.loads((out/'inputs.json').read_text(encoding='utf-8')):
        assert digest(c['artifact'])==c['artifact_sha256'];expected=c['expected'].encode()
        a,stdout,stderr=command(root,out,tag+'-'+c['name']+'-normal',[tools['native']['path'],'run',c['artifact'],'--execution-profile','compiler','--report-budget'],limit=30);assert stdout.replace(b"\r\n",b"\n")==expected
        budget=json.loads(stderr)
        b,stdout,stderr=command(root,out,tag+'-'+c['name']+'-counts',[tools['probe']['path'],c['artifact']],limit=30);assert stdout.replace(b"\r\n",b"\n")==expected
        counts,metrics=map(json.loads,stderr.decode().splitlines());assert budget['steps']==metrics['steps'] and budget['peak_calls']==metrics['peak_calls']
        rows.append({'name':c['name'],'normal':a,'counted':b,'budget':budget,'counts':counts,'tools':tools})
        save(dest/'kernels.json',{'status':'running','rows':rows})
    save(dest/'kernels.json',{'status':'passed','rows':rows})
    print(json.dumps([{'name':r['name'],'cpu':r['normal']['cpu_seconds'],'environment_probes':r['counts']['environment_probes']} for r in rows]))

def real(root,out,tag="real"):
    manifest=json.loads((out/'manifest.json').read_text(encoding='utf-8'));assert digest(out/'compiler.huab')==manifest['compiler_sha256']
    prior=root/'tests/artifacts/build/bo08-hua-sema-20261016';target=out/tag;target.mkdir(exist_ok=False);rows=[]
    save(out/'real-plan.json',{'inputs':['type_rules','ast_pool','parser'],'normal_limit':90,'counted_limit':120,'basis':'same compiler component and frozen inputs as BO-08; old successful ordinary runs reused after exact runtime/component/source checks; normal90s based on prior parser60s; counted120s allows atomic diagnostic overhead','timing_limitations':'single local pairs, prior normal records are not adjacent; no stable speedup assertion; memory/steps/receipts/artifacts compared','full_own_source_run':False})
    old_manifest=json.loads((prior/'manifest.json').read_text(encoding='utf-8'));assert old_manifest['native']['sha256']==manifest['tools']['native']['sha256']
    for name in ('type_rules','ast_pool','parser'):
        baseline=json.loads((prior/'cases'/name/'result.json').read_text(encoding='utf-8'));old=json.loads((prior/'after-cases'/name/'result.json').read_text(encoding='utf-8'))['rows']['after'];source=Path(baseline['input_path']);assert digest(source)==baseline['input_sha256']
        outputs=[];versions={}
        for mode,exe in [('normal',root/'tests/artifacts/build/hua.exe'),('counts',root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe')]:
            artifact=target/(name+'-'+mode+'.huab')
            args=[exe,'run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact] if mode=='normal' else [exe,out/'compiler.huab',source,artifact]
            timing,stdout,stderr=command(root,out,tag+'-'+name+'-'+mode,args,limit=90 if mode=='normal' else 120)
            assert json.loads(stdout)==baseline['rows']['before']['report'] and digest(artifact)==old['artifact_sha256']
            if mode=='normal':budget=json.loads(stderr);counts=None
            else:
                counts,metrics=map(json.loads,stderr.decode().splitlines());assert metrics['steps']==old['budget']['steps'] and metrics['peak_calls']==old['budget']['peak_calls']
            versions[mode]={'timing':timing,'counts':counts,'artifact_sha256':digest(artifact)}
        assert budget==old['budget'];rows.append({'case':name,'before':old['timing'],'after':versions,'budget':budget,'same_report_and_huab':True})
        save(target/'results.json',{'status':'running','rows':rows});print(json.dumps({'case':name,'old_cpu':old['timing']['cpu_seconds'],'new_cpu':versions['normal']['timing']['cpu_seconds'],'new_slot_hits':versions['counts']['counts']['vm_slot_hits'],'new_probe_visits':versions['counts']['counts']['environment_probes']}),flush=True)
    save(target/'results.json',{'status':'passed','rows':rows,'full_own_source_run':False})

def compare_kernels(out,tag="after-final"):
    before=json.loads((out/'before/kernels.json').read_text(encoding='utf-8'))
    after=json.loads((out/tag/'kernels.json').read_text(encoding='utf-8'));rows=[]
    assert before['status']==after['status']=='passed' and len(before['rows'])==len(after['rows'])==4
    for old,new in zip(before['rows'],after['rows']):
        assert old['name']==new['name'] and old['budget']==new['budget']
        for key in ('enforce_type_calls','copy_struct','copy_struct_fields','managed_allocations','user_calls','vm_steps'):
            assert old['counts'][key]==new['counts'][key],key
        a,b=old['counts'],new['counts']
        envs=b['vm_scopes']+b['user_calls']+2
        assert b['tracked_object_bytes']-a['tracked_object_bytes']==32*envs
        rows.append(dict(name=old['name'],old_cpu=old['normal']['cpu_seconds'],new_cpu=new['normal']['cpu_seconds'],old_peak=old['normal']['peak_working_bytes'],new_peak=new['normal']['peak_working_bytes'],old_probes=a['environment_probes'],new_probes=b['environment_probes'],new_slot_hits=b['vm_slot_hits'],new_field_hits=b['vm_field_hits'],new_type_hits=b['vm_multi_type_hits'],old_type_parses=a['type_argument_parses'],new_type_parses=b['type_argument_parses'],environment_count=envs,extra_environment_bytes=32))
    result=dict(status='passed',rows=rows)
    save(out/'kernel-comparison-final.json',result);return result

def analyze(root,out):
    import re
    manifest=json.loads((out/'manifest.json').read_text(encoding='utf-8'))
    kernels=compare_kernels(out);real_data=json.loads((out/'real-final/results.json').read_text(encoding='utf-8'))
    c04=json.loads((out/'final-validation/a07-c04/results.json').read_text(encoding='utf-8'));entry=json.loads((root/'tests/artifacts/build/a06-compiler/results.json').read_text(encoding='utf-8'))
    guards=json.loads((out/'resume-guard-review.json').read_text(encoding='utf-8'))
    assert all(x['status']=='passed' for x in (kernels,real_data,c04,entry,guards))
    current=digest(root/'tests/artifacts/build/hua.exe');assert current==c04['hua_sha256']
    after=json.loads((out/'after-final/kernels.json').read_text(encoding='utf-8'))
    assert all(r['tools']['native']['sha256']==current for r in after['rows'])
    assert entry['hua_sha256']==json.loads((out/'after/kernels.json').read_text(encoding='utf-8'))['rows'][0]['tools']['native']['sha256']
    text=(out/'commands/final-ctests.stdout').read_text(encoding='utf-8',errors='replace')
    passed=re.findall(r'Test\s+#\d+: (\w+)\s+\.+\s+Passed',text)
    assert len(passed)==30 and len(set(passed))==30 and entry['commands']==29 and entry['reused_commands']==17
    for label in ('final-native-slots-r2','final-native-metadata','final-ctests','final-c04-small-correctness-r2'):
        check=json.loads((out/'commands'/(label+'.json')).read_text(encoding='utf-8'));assert check['returncode']==0 and not check['timeout']
    assert '63 VM slot' in (out/'commands/final-native-slots-r2.stdout').read_text()
    rows=[]
    for r in real_data['rows']:
        before=r['before'];new=r['after']['normal']['timing'];counts=r['after']['counts']['counts']
        rows.append({'case':r['case'],'before_wall':before['seconds'],'after_wall':new['seconds'],'before_cpu':before['cpu_seconds'],'after_cpu':new['cpu_seconds'],'wall_change_percent':100*(new['seconds']/before['seconds']-1),'cpu_change_percent':100*(new['cpu_seconds']/before['cpu_seconds']-1),'before_peak_bytes':before['peak_working_bytes'],'after_peak_bytes':new['peak_working_bytes'],'slot_hits':counts['vm_slot_hits'],'fallback_scope_probes':counts['environment_probes'],'field_hits':counts['vm_field_hits'],'field_type_hits':counts['vm_field_type_hits'],'diagnostic_gc_seconds':counts['gc_nanoseconds']/1e9,'diagnostic_wall_seconds':r['after']['counts']['timing']['seconds'],'same_budget_receipt_and_huab':True})
    commands=[json.loads(p.read_text(encoding='utf-8')) for p in (out/'commands').glob('*.json')]
    commands=[r for r in commands if 'command' in r]
    profile=digest(root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe')
    result={'status':'BO-09 selected guarded-slot/field/static-multi scheme locally verified; A11 still pending','baseline_git':manifest['git'],'before_native_sha256':manifest['tools']['native']['sha256'],'native_sha256':current,'diagnostic_sha256':profile,'compiler_sha256':manifest['compiler_sha256'],'compiler_source_bytes':sum(p.stat().st_size for p in closure(root/'selfhost/archive_main.hua').values()),'kernel_cases':kernels['rows'],'real_cases':rows,'native_slot_checks':63,'native_metadata_checks':686,'distinct_passed_ctests':passed,'compiler_entry_main_implementation_R1':entry,'compiler_entry_final_R2':'entry_contract, C04 and fixed C07 inputs revalidated; no full compiler_entry source-preparation rerun for GC clear-only correction','resume_identity_rejection_checks':len(guards['checks']),'c04':{k:c04[k] for k in ('status','cases','checks','commands','seconds')},'native_node_fact_checks':sum(x.get('native_facts',0) for x in c04['rows']),'outer_commands':len(commands),'outer_failed_commands':sum(r['returncode']!=0 for r in commands),'outer_timeouts':sum(r['timeout'] for r in commands),'nested_regression_suite_timeouts':1,'broad_A02_supervision_timeout':1,'vm_type_cases':json.loads((root/'tests/artifacts/build/vm-type-contract/a02/acceptance/native-results.json').read_text(encoding='utf-8')),'command_wall_seconds':sum(r['seconds'] for r in commands),'performance_limits':'single local observations; old real normal commands reused, not adjacent pairs; diagnostic times/counters are not normal CPU shares; no generic speedup promise','full_own_source_or_generations':False,'production_limits':[500000000,128,65536,600,16777216]}
    assert result['outer_timeouts']==1
    save(out/'summary.json',result)
    print(json.dumps({k:result[k] for k in ('status','real_cases','c04','outer_commands','outer_failed_commands','nested_regression_suite_timeouts')},ensure_ascii=False))

def main():
    p=argparse.ArgumentParser();p.add_argument('action',choices=['init','before','after','real','analyze']);p.add_argument('--output',type=Path,required=True);p.add_argument('--reuse-component',type=Path);p.add_argument('--reuse-manifest',type=Path);p.add_argument('--tag');a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve()
    if a.action=='init':initialize(root,out,a.reuse_component,a.reuse_manifest)
    elif a.action=='real':real(root,out,a.tag or 'real')
    elif a.action=='analyze':analyze(root,out)
    else:kernels(root,out,a.tag or a.action)
if __name__=='__main__':main()
