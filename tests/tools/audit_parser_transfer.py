"""BO-03: bounded parser transfer audit; no compiler own-source or generation run."""
from pathlib import Path
import argparse, json, os, shutil, subprocess, statistics
import audit_bootstrap_phases as audit
from audit_bootstrap_phases import WATCHES, closure, command, digest, reduce_events, save

def initialize(root,out,baseline_ref):
    assert not out.exists(), 'Use a fresh output directory'
    files=subprocess.check_output(['git','ls-tree','-r','--name-only',baseline_ref,'--','selfhost'],cwd=root,text=True).splitlines()
    sources=[name for name in files if name.endswith('.hua')]; assert sources
    out.mkdir(); frozen=out/'before-source'; frozen.mkdir()
    for name in sources:
        (frozen/Path(name).name).write_bytes(subprocess.check_output(['git','show',baseline_ref+':'+name],cwd=root))
    native=root/'tests/artifacts/build/hua.exe'; probe=root/'tests/artifacts/build-a11-profile/hua_performance_probe.exe'
    shutil.copy2(probe,out/'probe.exe')
    save(out/'runtime-identities.json',{'native':digest(native),'probe':digest(probe),'oracle':digest(root/'tests/artifacts/build/hua_parser_oracle.exe')})
    save(out/'baseline-manifest.json',{'baseline_ref':baseline_ref,'compiler_sources':{name:digest(path) for name,path in closure(frozen/'archive_main.hua').items()},'all_sources':{p.name:digest(p) for p in frozen.glob('*.hua')},'full_own_source_run':False})

def candidate_parser(before,tag):
    signature='fn peek(s State) lexer.Token { return s.data[s.pos] }'
    assert before.count(signature)==1
    if tag=='after':
        value=before.replace(signature,'# Only the token view and cursor are needed; avoid copying Pool/SourceId/Limits.\nfn peek(data []lexer.Token, pos int) lexer.Token { return data[pos] }')
        return value.replace('peek(s)','peek(s.data,s.pos)')
    value=before.replace(signature,'# Read tokens at the use site; local bindings still snapshot tokens before writes.')
    value=value.replace('peek(s)','s.data[s.pos]')
    if tag=='queries':
        old='fn suffix(s mut State, left ast.NodeId, multiline bool) Result<ast.NodeId> {\n    let t = s.data[s.pos]\n    let old = ast.get(s.pool,left)?'
        new='fn suffix(s mut State, left ast.NodeId, multiline bool) Result<ast.NodeId> {\n    let t = s.data[s.pos]\n    # left is already a validated parser node. Ordinary lookahead needs no Node/Span copy.\n    if t.kind != "." && t.kind != "[" && t.kind != "++" && t.kind != "--" && t.kind != "?" && t.kind != "{" { return ok(left) }\n    let old = ast.get(s.pool,left)?'
        assert value.count(old)==1; value=value.replace(old,new)
    return value

def observed_component(root,out,source,target,tag):
    observed=target/'observed-source';shutil.copytree(source,observed)
    for name,watches in WATCHES.items():
        path=observed/name;value=path.read_text(encoding='utf-8')
        for signature,phase in watches:
            assert value.count(signature)==1,(name,signature)
            value=value.replace(signature,signature+'\n    print("@@hua.phase:begin:'+phase+'")\n    defer _bo_phase_end("'+phase+'")\n',1)
        value+='\nfn _bo_phase_end(label string) { print("@@hua.phase:end:"+label) }\n'
        path.write_text(value,encoding='utf-8',newline='\n')
    command(root,out,'prepare-'+tag+'-observed',[root/'tests/artifacts/build/hua.exe','build',observed/'archive_main.hua','-o',target/'observed.huab'],90)

def prepare(root, out, tag):
    target = out/tag
    assert not target.exists(), 'Use a fresh tag; previous evidence is immutable'
    target.mkdir()
    if tag == 'before':
        source=out/'before-source'
        prior=root/'tests/artifacts/build/bo00-01-20261016'
        manifest=json.loads((prior/'baseline-manifest.json').read_text(encoding='utf-8')) if (prior/'baseline-manifest.json').exists() else None
        hashes={name:digest(path) for name,path in closure(source/'archive_main.hua').items()}
        if manifest and hashes==manifest['compiler_sources']:
            assert digest(prior/'baseline-component.huab')==manifest['baseline_component']['sha256']
            observed=json.loads((prior/'diagnostic-manifest.json').read_text(encoding='utf-8'))
            assert digest(prior/'phase-component.huab')==observed['phase_component_sha256']
            shutil.copy2(prior/'baseline-component.huab',target/'compiler.huab')
            shutil.copy2(prior/'phase-component.huab',target/'observed.huab')
        else:
            command(root,out,'prepare-before-compiler',[root/'tests/artifacts/build/hua.exe','build',source/'archive_main.hua','-o',target/'compiler.huab'],90)
            observed_component(root,out,source,target,tag)
    else:
        source=out/(tag+'-source');shutil.copytree(out/'before-source',source)
        path=source/'parser.hua';path.write_text(candidate_parser(path.read_text(encoding='utf-8'),tag),encoding='utf-8',newline='\n')
        command(root,out,'prepare-'+tag+'-compiler',[root/'tests/artifacts/build/hua.exe','build',source/'archive_main.hua','-o',target/'compiler.huab'],90)
        observed_component(root,out,source,target,tag)
    command(root,out,'prepare-'+tag+'-parser',[root/'tests/artifacts/build/hua.exe','build',source/'parser_extended_main.hua','-o',target/'parser.huab'],60)
    save(target/'identities.json',{'source_hashes':{p.name:digest(p) for p in source.glob('*.hua')},'compiler':digest(target/'compiler.huab'),'observed':digest(target/'observed.huab'),'parser':digest(target/'parser.huab')})

def inputs(root,out):
    target=out/'inputs'
    if not target.exists():
        target.mkdir()
        for name in ('catalog','type_rules'):
            shutil.copy2(out/'before-source'/(name+'.hua'),target/(name+'.hua'))
        for n in (16,64):
            (target/('declarations-'+str(n)+'.hua')).write_text(''.join(f'fn value{i}(x int) int {{return x+{i}}}\n' for i in range(n)),encoding='utf-8',newline='\n')
    return sorted(target.glob('*.hua'))

def measure(root,out,tag):
    native=root/'tests/artifacts/build/hua.exe'; probe=out/'probe.exe'; target=out/tag
    identities=json.loads((out/'runtime-identities.json').read_text(encoding='utf-8'))
    assert digest(native)==identities['native'] and digest(probe)==identities['probe']
    artifacts=json.loads((target/'identities.json').read_text(encoding='utf-8'))
    for key,file in [('compiler','compiler.huab'),('observed','observed.huab')]: assert digest(target/file)==artifacts[key]
    data={'status':'running','tag':tag,'identities':identities,'rows':[],'full_own_source_run':False}
    save(target/'results.json',data)
    try:
        for source in inputs(root,out):
            name=source.stem; artifact=target/(name+'.huab'); observed=target/(name+'-observed.huab')
            normal,report,_=command(root,out,tag+'-'+name+'-normal',[native,'run',target/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact])
            events=target/(name+'-events.jsonl')
            diagnostic,diagout,stderr=command(root,out,tag+'-'+name+'-observed',[probe,'--phase-events',events,target/'observed.huab',source,observed])
            assert report==diagout and artifact.read_bytes()==observed.read_bytes(),name
            receipt=json.loads(report); assert receipt['status']=='ok',receipt
            trace=[json.loads(line) for line in events.read_text(encoding='utf-8').splitlines()]
            phases=reduce_events(trace); counts=trace[-1]['counters']
            assert counts==json.loads(stderr.decode().splitlines()[0])
            row={'case':name,'source_bytes':source.stat().st_size,'source_sha256':digest(source),'artifact_sha256':digest(artifact),'report_sha256':__import__('hashlib').sha256(report).hexdigest(),'normal':normal,'diagnostic':diagnostic,'phases':phases,'counts':counts}
            if tag!='before':
                old=next(r for r in json.loads((out/'before/results.json').read_text(encoding='utf-8'))['rows'] if r['case']==name)
                assert row['source_sha256']==old['source_sha256'] and row['report_sha256']==old['report_sha256'] and row['artifact_sha256']==old['artifact_sha256'],name
                prior=old['phases']['parse']['struct_copies']; current=phases['parse']['struct_copies']
                keys={}
                def entry(hist,suffix):
                    if suffix not in keys:
                        matches=[k for k,v in prior.items() if k.endswith('$'+suffix) and v['objects']>0]
                        assert len(matches)==1,(suffix,matches)
                        keys[suffix]=matches[0]
                    # Struct basenames are not identities: capabilities also defines State.
                    return hist[keys[suffix]]
                removed=entry(prior,'State')['objects']-entry(current,'State')['objects']
                assert removed>0
                if tag in ('after','inline'):
                    for kind,width in [('State',13),('Pool',3),('SourceId',2),('Limits',5)]:
                        assert entry(prior,kind)['objects']-entry(current,kind)['objects']==removed,kind
                        assert entry(prior,kind)['fields']-entry(current,kind)['fields']==removed*width,kind
                    if tag=='after': assert counts['user_calls']==old['counts']['user_calls']
                    total_removed=entry(old['counts']['struct_copies'],'State')['objects']-entry(counts['struct_copies'],'State')['objects']
                    assert total_removed==removed+4
                    if tag=='after':
                        assert old['counts']['copy_struct_fields']-counts['copy_struct_fields']==total_removed*23
                        assert old['counts']['copy_struct']-counts['copy_struct']==total_removed*4
                        assert old['counts']['managed_allocations']-counts['managed_allocations']==total_removed*4
                        assert counts['vm_steps']-old['counts']['vm_steps']==total_removed
                    else:
                        assert old['counts']['user_calls']-counts['user_calls']==total_removed
                        assert old['counts']['copy_struct_fields']-counts['copy_struct_fields']>total_removed*23
                        assert old['counts']['managed_allocations']-counts['managed_allocations']>total_removed*4
                        assert counts['vm_steps']<old['counts']['vm_steps']
                        assert entry(old['counts']['struct_copies'],'Token')['objects']>entry(counts['struct_copies'],'Token')['objects']
                    row['total_peek_state_copies_removed']=total_removed
                    row['peek_state_copies_removed']=removed
                else:
                    intermediate=next(r for r in json.loads((out/'inline/results.json').read_text(encoding='utf-8'))['rows'] if r['case']==name) if (out/'inline/results.json').exists() else old
                    for metric in ('copy_struct_fields','copy_struct','managed_allocations','vm_steps','user_calls'):
                        assert counts[metric]<intermediate['counts'][metric]<=old['counts'][metric],metric
                    for kind in ('Node','SourceSpan'):
                        assert entry(current,kind)['objects']<entry(intermediate['phases']['parse']['struct_copies'],kind)['objects'],kind
                    row['parse_State_copies_reduced']=removed
                row['exclusive_parse_reduction']=1-phases['parse']['ns']/old['phases']['parse']['ns']
                row['normal_wall_reduction']=1-normal['seconds']/old['normal']['seconds']
            data['rows'].append(row);save(target/'results.json',data)
        data['status']='passed';save(target/'results.json',data)
    except BaseException as failure:
        data['status']='stopped';data['error']=repr(failure);save(target/'results.json',data);raise

def oracle(root,out,tag):
    identities=json.loads((out/'runtime-identities.json').read_text(encoding='utf-8'))
    assert digest(root/'tests/artifacts/build/hua.exe')==identities['native']
    if 'oracle' in identities: assert digest(root/'tests/artifacts/build/hua_parser_oracle.exe')==identities['oracle']
    target=out/'parser-oracle'
    target.mkdir(exist_ok=True)
    assert not (target/('results-'+tag+'.json')).exists(), 'Oracle results already exist'
    cases=[('precedence','var x=1+2*3\nx+=4\n'),('nested-types','let xs List<List<int>>=[]\nlet ys Result<int,string>=ok(1)\n'),('suffix','var a [2][2]u8=[[1,2],[3,4]]\na[0][1:]\na[0][0]++\n'),('qualified','import pkg as p\nlet x=p.Point{x:1}.x\n'),('control','fn f(x int) int{if x>0{return x}\nwhile x<0{x+=1}\nreturn x}\n'),('unicode','\ufeff#* 中文😀 *#\r\nlet text="😀"\r'),('trailing','let a=[1,2,]\n'),('bad-slice','a[1 2]\n'),('bad-target','1=2\n'),('bad-string','"unterminated'),('source-budget','#'+('x'*8200)+'\n'),('nesting-budget','('*20+'1'+')'*20+'\n')]
    if tag=='queries':
        cases.extend([('propagation','fn f(){g()?}\n'),('bad-propagation','g()?\n'),('header-braces','if p.ready {p.value++} else {p.value--}\n'),('bad-member','a.\n')])
    rows=[]
    for name,text in cases:
        source=target/(name+'.hua')
        if source.exists(): assert source.read_bytes()==text.encode('utf-8')
        else: source.write_bytes(text.encode('utf-8'))
        outputs=[]
        for version in ('before',tag):
            if tag in ('inline','queries') and version=='before' and (out/'commands'/('oracle-'+name+'-before.stdout')).exists():
                output=(out/'commands'/('oracle-'+name+'-before.stdout')).read_bytes()
            else:
                _,output,_=command(root,out,'oracle-'+name+'-'+version,[root/'tests/artifacts/build/hua.exe','run',out/version/'parser.huab','--execution-profile','compiler','--',source,'input.hua'])
            outputs.append(json.loads(output))
        assert outputs[0]==outputs[1],(name,outputs)
        report=outputs[1]
        if 'budget' in name:
            assert report['status']=='error' and report['error'].startswith('E8202@'),report
        else:
            if tag in ('inline','queries') and (out/'commands'/('oracle-'+name+'-native.stdout')).exists(): raw=(out/'commands'/('oracle-'+name+'-native.stdout')).read_bytes()
            else: _,raw,_=command(root,out,'oracle-'+name+'-native',[root/'tests/artifacts/build/hua_parser_oracle.exe',source])
            ref=json.loads(raw)
            if ref['status']=='ok':
                assert report['status']=='ok',report
                projected=[{k:n[k] for k in ('kind','text','start','end','line','column','depth')}|{'arity':len(n['children'])} for n in report['nodes']]
                assert projected==ref['nodes'],(name,projected,ref)
            else:
                assert report['status']=='error' and report['error'].startswith(f"{ref['code']}@{ref['line']}:{ref['column']}"),(name,report,ref)
        rows.append({'case':name,'status':'passed','parser_stats':report.get('stats'),'diagnostic':report.get('error')});save(target/('results-'+tag+'.json'),{'status':'running','rows':rows})
    save(target/('results-'+tag+'.json'),{'status':'passed','cases':len(rows),'independent_structure_or_diagnostic_cases':len(rows)-2,'budget_cases':2,'full_reports_equal':True,'rows':rows})

def paired(root,out):
    native=root/'tests/artifacts/build/hua.exe';probe=out/'probe.exe';source=out/'inputs/type_rules.hua';target=out/'paired';assert not target.exists();target.mkdir()
    identities=json.loads((out/'runtime-identities.json').read_text(encoding='utf-8'))
    assert digest(native)==identities['native'] and digest(probe)==identities['probe']
    rows=[];save(target/'plan.json',{'purpose':'Distinguish global between-batch clock variation from the two local changes; prior query batch slowed unchanged phases too','input_bytes':source.stat().st_size,'rounds':3,'versions':['before','inline','queries'],'calls':18,'full_own_source_run':False,'no_more_candidate_changes':True})
    for index,order in enumerate([('before','inline','queries'),('queries','before','inline'),('inline','queries','before')]):
     for tag in order:
      stem=f'paired-{index}-{tag}';artifact=target/(stem+'.huab');observed=target/(stem+'-observed.huab');tracefile=target/(stem+'-events.jsonl')
      normal,report,_=command(root,out,stem+'-normal',[native,'run',out/tag/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact])
      diag,diagout,_=command(root,out,stem+'-diagnostic',[probe,'--phase-events',tracefile,out/tag/'observed.huab',source,observed])
      expected=out/tag/'type_rules.huab';assert artifact.read_bytes()==observed.read_bytes()==expected.read_bytes()
      previous=json.loads((out/tag/'results.json').read_text(encoding='utf-8'));old=next(r for r in previous['rows'] if r['case']=='type_rules')
      import hashlib
      assert report==diagout and hashlib.sha256(report).hexdigest()==old['report_sha256']
      trace=[json.loads(line) for line in tracefile.read_text(encoding='utf-8').splitlines()];phases=reduce_events(trace);counts=trace[-1]['counters']
      for key in ('vm_steps','user_calls','copy_struct_fields','managed_allocations'):assert counts[key]==old['counts'][key],key
      rows.append({'round':index,'tag':tag,'normal':normal,'diagnostic':diag,'phases':phases});save(target/'results.json',{'status':'running','rows':rows})
    summary={}
    for tag in ('before','inline','queries'):
     group=[r for r in rows if r['tag']==tag]
     summary[tag]={'normal_wall_median':statistics.median(r['normal']['seconds'] for r in group),'normal_cpu_median':statistics.median(r['normal']['cpu_seconds'] for r in group),'normal_wall_range':[min(r['normal']['seconds'] for r in group),max(r['normal']['seconds'] for r in group)],'parse_exclusive_median':statistics.median(r['phases']['parse']['ns']/1e9 for r in group),'unchanged_types_median':statistics.median(r['phases']['types']['ns']/1e9 for r in group),'parse_exclusive_range':[min(r['phases']['parse']['ns']/1e9 for r in group),max(r['phases']['parse']['ns']/1e9 for r in group)]}
    save(target/'results.json',{'status':'passed','rows':rows,'summary':summary,'calls':18,'full_own_source_run':False})

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('init','prepare','measure','oracle','paired'));p.add_argument('--baseline-ref',default='a9c5f6a');p.add_argument('--tag',choices=('before','after','inline','queries'));p.add_argument('--resume',action='store_true');p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();root=a.root.resolve();out=a.output.resolve();assert out.is_relative_to(root/'tests/artifacts') and out!=root/'tests/artifacts'
    os.environ['PATH']=str(root/'tests/artifacts/build')+os.pathsep+os.environ.get('PATH','')
    if a.resume:
        assert a.mode=='measure' and a.tag
        ledger=[]
        for path in sorted((out/'commands').glob(a.tag+'-*.json')):
            row=json.loads(path.read_text(encoding='utf-8'))
            assert row['returncode']==0 and not row['timeout'],path
            ledger.append({'label':row['label'],'command_sha256':digest(path),'stdout_sha256':digest(path.with_suffix('.stdout')),'stderr_sha256':digest(path.with_suffix('.stderr'))})
        save(out/(a.tag+'-resume-ledger.json'),{'reason':'Fix histogram selection for unrelated zero-count Limits and count the empty seed outside the parse wrapper; reuse completed calls after runtime/component/input identity checks','completed':ledger})
        audit.RESUME={'completed_labels':{row['label'] for row in ledger}}
    if a.mode=='init': initialize(root,out,a.baseline_ref)
    elif a.mode=='paired': paired(root,out)
    elif a.mode=='oracle': oracle(root,out,a.tag or 'queries')
    else:
        assert a.tag,'tag required'
        if a.mode=='measure': assert a.resume or not (out/a.tag/'results.json').exists(), 'Results exist; use explicit resume to preserve completed calls'
        (prepare if a.mode=='prepare' else measure)(root,out,a.tag)
if __name__=='__main__':main()
