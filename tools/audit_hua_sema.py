"""BO-08 continuation: observe the existing Hua capability scheduler on bounded inputs.
Only diagnostic copies are instrumented; production execution limits stay unchanged.
"""
from pathlib import Path
import argparse,json,shutil,subprocess,os
from audit_bootstrap_phases import command,save,digest,closure,machine,WATCHES

PREFIX="@@hua.sema:"

def replace(text,old,new,n=1):
    assert text.count(old)==n,(old,text.count(old),n)
    return text.replace(old,new)

def instrument(text):
    text=replace(text,'import std.json as json','import std.json as json\nimport std.time as time')
    helpers='var _bo_counts=map[string]int{}\nfn _bo_add(name string, count int) { _bo_counts[name]=unwrap_or(_bo_counts[name],0)+count }\nfn _bo_end(name string, start int) { _bo_add(name+"_ns",time.monotonic_ns()-start) }\nfn _bo_dump() { print("@@hua.sema:"+unwrap(json.encode(_bo_counts))) }\n'
    text=replace(text,'pub struct View {',helpers+'pub struct View {')
    # Timings include nested calls and diagnostic overhead; never add them as exclusive CPU shares.
    signatures={
      'same_fields':'a map[string]View, b map[string]View) bool',
      'same_summary':'a Summary, b Summary) bool',
      'snapshot':'values map[string]Value) map[string]Value',
      'editable':'e mut Environment, input Value, owner string) Value',
      'storage_aliases':'e mut Environment, container View) Result<[]string>',
      'field_defaults':'s mut State, base View, level int) map[string]View',
      'merge':'a Value, b Value) Value',
      'instantiate':'caller string, module string, node int, source Value, actual map[string]Value) Value',
      'instantiate_view':'caller string, module string, node int, source View, actual map[string]Value) View'}
    for name,signature in signatures.items():
        token='fn '+name+'('+signature+' {'
        text=replace(text,token,token+'\n    _bo_add("'+name+'_calls",1)\n    let _bo_start=time.monotonic_ns()\n    defer _bo_end("'+name+'",_bo_start)\n')
    text=replace(text,'    if len(a)>8 { let left=json.encode(a)','    if len(a)>8 { _bo_add("compare_json",1)\n        _bo_add("compare_json_fields",len(a)+len(b))\n        let left=json.encode(a)')
    text=replace(text,'        if is_ok(left) && is_ok(right) { return unwrap(left)==unwrap(right) }','        if is_ok(left) && is_ok(right) { _bo_add("compare_json_bytes",len(unwrap(left))+len(unwrap(right)))\n            return unwrap(left)==unwrap(right) }\n        _bo_add("compare_json_fallback",1)')
    text=replace(text,'    for name, input in a { if !has(b,name) || !same_view(input,unwrap(b[name])) { return false } }','    for name, input in a { _bo_add("compare_element_visits",1)\n        if !has(b,name) || !same_view(input,unwrap(b[name])) { return false } }')
    text=replace(text,'    for name, input in values { out[name]=input }','    _bo_add("snapshot_bindings",len(values))\n    for name, input in values { _bo_add("snapshot_fields",len(input.fields))\n        _bo_add("snapshot_positions",len(input.positions))\n        out[name]=input }')
    text=replace(text,'    if e.updating && has(e.edited_fields,owner) {','    if e.updating && has(e.edited_fields,owner) { _bo_add("editable_batch_hit",1)')
    text=replace(text,'    out=clone(input)\n    if e.updating','    _bo_add("editable_detach",1)\n    _bo_add("editable_fields",len(input.fields))\n    out=clone(input)\n    if e.updating')
    text=replace(text,'    if !has(s.shapes,cache_key) {','    if !has(s.shapes,cache_key) { _bo_add("shape_miss",1)\n')
    text=replace(text,'    for path, fact in unwrap(s.shapes[cache_key]) {','    _bo_add("shape_instantiated_fields",len(unwrap(s.shapes[cache_key])))\n    for path, fact in unwrap(s.shapes[cache_key]) {')
    text=replace(text,'if has(e.alias_cache,identity) { return ok(unwrap(e.alias_cache[identity])) }','if has(e.alias_cache,identity) { _bo_add("alias_hit",1)\n                return ok(unwrap(e.alias_cache[identity])) }')
    text=replace(text,'    for owner, root in e.values {','    _bo_add("alias_scans",1)\n    for owner, root in e.values { _bo_add("alias_bindings",1)\n        _bo_add("alias_fields",len(root.fields))')
    text=replace(text,'    var passes=0\n    var changed=true','    var passes=0\n    var changed=true\n    _bo_counts=map[string]int{}\n    let _bo_cap_start=time.monotonic_ns()')
    text=replace(text,'            if !has(s.dirty,name) { continue }','            if !has(s.dirty,name) { _bo_add("pass-"+str(passes)+"-skip",1)\n                continue }\n            _bo_add("pass-"+str(passes)+"-visit",1)')
    text=replace(text,'{ changed=true\n                s.summaries[name]=next','{ changed=true\n                _bo_add("pass-"+str(passes)+"-changed",1)\n                s.summaries[name]=next')
    text=replace(text,'    if changed { for name, input in s.summaries {','    if changed { _bo_add("summary_fallback",1)\n        for name, input in s.summaries {')
    text=replace(text,'        if has(s.dirty,name) { delete(s.errors,name)','        if has(s.dirty,name) { _bo_add("validation_visit",1)\n            delete(s.errors,name)')
    text=replace(text,'    return ok(Model{facts:s.facts,passes:passes,work:s.work,error:s.error})','    _bo_add("capabilities_function_ns",time.monotonic_ns()-_bo_cap_start)\n    _bo_add("passes",passes)\n    _bo_add("work",s.work)\n    _bo_add("functions",len(functions))\n    _bo_dump()\n    return ok(Model{facts:s.facts,passes:passes,work:s.work,error:s.error})')
    return text

def stages(copied):
    for name,watch in WATCHES.items():
        p=copied/name;text=p.read_text(encoding='utf-8')
        if 'import std.time as time' not in text:text='import std.time as time\n'+text
        for signature,label in watch:
            text=replace(text,signature,signature+'\n    _bo_stage("begin","'+label+'")\n    defer _bo_stage("end","'+label+'")\n')
        text+='\nfn _bo_stage(event string, phase string) { print("@@hua.stage:"+event+"|"+phase+"|"+str(time.monotonic_ns())) }\n'
        p.write_text(text,encoding='utf-8',newline='\n')

def init(root,out,reuse=None,reuse_manifest=None):
    assert not out.exists(),'choose a fresh output directory'
    out.mkdir(parents=True)
    sources=closure(root/'selfhost/archive_main.hua'); copied=out/'diagnostic-source';copied.mkdir()
    for name,path in sources.items():shutil.copy2(path,copied/name)
    cap=copied/'capabilities.hua';cap.write_text(instrument(cap.read_text(encoding='utf-8')),encoding='utf-8',newline='\n')
    before=out/'before-source';before.mkdir()
    for name,path in sources.items():shutil.copy2(path,before/name)
    native=root/'build/hua.exe'
    if reuse:
        assert reuse_manifest,'a reused component needs its source/hash manifest'
        old=json.loads(reuse_manifest.read_text(encoding='utf-8'))
        assert old.get('source_hashes',old.get('sources'))=={k:digest(v) for k,v in sources.items()}
        assert digest(reuse)==old['component_sha256']
        shutil.copy2(reuse,out/'before.huab')
    else:
        command(root,out,'prepare-before',[native,'build',before/'archive_main.hua','-o',out/'before.huab'],limit=90)
    stages(copied)
    save(out/'manifest.json',{'git':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'native':{'path':str(native),'sha256':digest(native)},'component_sha256':digest(out/'before.huab'),'source_hashes':{k:digest(v) for k,v in sources.items()},'diagnostic_sources':{p.name:digest(p) for p in copied.glob('*.hua')},'host':machine(),'limits':[500000000,128,65536,600,16777216],'full_own_source_run':False,'timing_scope':'inclusive diagnostic wall timers, overlapping; not production CPU shares'})
    save(out/'plan.json',{'hypothesis':'measure existing dirty scheduling, comparison, immutable metadata snapshots and cache hit/scan scales before selecting one production change','inputs':'calibration scalar; independent dimensions for fields/functions; catalog and type_rules real modules; stop on first diagnostic failure','watchdogs':'90s C0 build, initially30s small VM diagnostics; increase only after a smaller completed size supplies evidence; no own-source VM run','expected':'exact C07 reports and output HUAB match uninstrumented compiler; field comparison semantic oracle before any change'})
    command(root,out,'prepare-diagnostic',[native,'build',copied/'archive_main.hua','-o',out/'diagnostic.huab'],limit=90)
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'));m['diagnostic_sha256']=digest(out/'diagnostic.huab');save(out/'manifest.json',m)

def stage_totals(lines):
    records=[line[len('@@hua.stage:'):].split('|') for line in lines if line.startswith('@@hua.stage:')]
    stack=[];totals={};previous=None
    for event,phase,stamp in records:
        now=int(stamp)
        assert previous is None or now>=previous,'stage clock went backwards'
        if previous is not None:
            owner=stack[-1] if stack else 'between-stages';totals[owner]=totals.get(owner,0)+now-previous
        if event=='begin':stack.append(phase)
        else:
            assert event=='end' and stack and stack[-1]==phase,'stage nesting mismatch'
            stack.pop()
        previous=now
    assert not stack,'incomplete stage trace'
    if records:assert sum(totals.values())==int(records[-1][2])-int(records[0][2])
    return totals

def run(root,out,name,path,limit=30):
    assert name and path and 0<limit<=600
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'));native=Path(m['native']['path']);assert digest(native)==m['native']['sha256']
    assert digest(out/'before.huab')==m['component_sha256']
    if 'diagnostic_sha256' in m:assert digest(out/'diagnostic.huab')==m['diagnostic_sha256']
    p=Path(path).resolve();target=out/'cases'/name;target.mkdir(parents=True,exist_ok=False)
    rows={};output={}
    for tag,component in [('before',out/'before.huab'),('diagnostic',out/'diagnostic.huab')]:
        artifact=target/(tag+'.huab')
        row,stdout,stderr=command(root,out,name+'-'+tag,[native,'run',component,'--execution-profile','compiler','--report-budget','--',p,artifact],limit=limit)
        lines=stdout.decode('utf-8').splitlines();marks=[json.loads(s[len(PREFIX):]) for s in lines if s.startswith(PREFIX)]
        reports=[json.loads(s) for s in lines if not s.startswith(PREFIX) and not s.startswith('@@hua.stage:')];assert len(reports)==1 and reports[0]['status']=='ok',reports
        assert len(marks)==(1 if tag=='diagnostic' else 0)
        output[tag]={'report':reports[0],'artifact_sha256':digest(artifact)};rows[tag]={'timing':row,'budget':json.loads(stderr),'counts':marks[0] if marks else None,'exclusive_stage_ns':stage_totals(lines),**output[tag]}
    assert output['before']==output['diagnostic'],name
    save(target/'result.json',{'status':'passed','input_sha256':digest(p),'input_path':str(p),'input_sources':{k:digest(v) for k,v in closure(p).items()},'rows':rows,'same_report_and_huab':True})
    print(json.dumps({'case':name,'counts':rows['diagnostic']['counts'],'normal_cpu':rows['before']['timing']['cpu_seconds']},indent=2))

def analyze(root,out):
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'))
    assert digest(m['native']['path'])==m['native']['sha256']
    matrix=json.loads((out/'integration.json').read_text(encoding='utf-8'));assert matrix['status']=='passed'
    kernel=json.loads((out/'kernels/comparison.json').read_text(encoding='utf-8'));assert kernel['status']=='passed'
    c04=json.loads((out/'a07-c04/results.json').read_text(encoding='utf-8'));assert c04['status']=='passed'
    models=json.loads((out/'offline-model-review.json').read_text(encoding='utf-8'));assert models['status']=='passed'
    old=json.loads((out/'cases/parser/result.json').read_text(encoding='utf-8'));new=json.loads((out/'after-cases/parser/result.json').read_text(encoding='utf-8'))
    counts=old['rows']['diagnostic']['counts'];visits=sum(counts.get(f'pass-{n}-visit',0) for n in range(1,counts['passes']+1));skips=counts['passes']*counts['functions']-visits
    commands=[]
    for folder in (out/'commands',out/'kernels/commands'):
        for file in folder.glob('*.json'):
            row=json.loads(file.read_text(encoding='utf-8'))
            if 'command' in row:commands.append(row)
    result={'status':'local correctness and bounded copy-work reduction passed; overall performance acceptance NOT passed','baseline_git':m['git'],'native_sha256':m['native']['sha256'],'component_before_sha256':digest(out/'before.huab'),'component_after_sha256':digest(out/'after.huab'),'current_source_hashes':{k:digest(v) for k,v in closure(root/'selfhost/archive_main.hua').items()},'current_source_bytes':sum(p.stat().st_size for p in closure(root/'selfhost/archive_main.hua').values()),'same_input_cases':len(matrix['rows']),'kernel_cases':len(kernel['rows']),'kernel_rows':kernel['rows'],'offline_model_checks':models['checks'],'c04':{k:c04[k] for k in ('cases','checks','commands','seconds','status')},'parser_scheduler':{'functions':counts['functions'],'passes':counts['passes'],'visits':visits,'skips':skips,'alias_cache_returns':counts['alias_hit'],'alias_scans':counts['alias_scans'],'summary_fallbacks':counts.get('summary_fallback',0)},'parser_normal_before':old['rows']['before']['timing'],'parser_normal_after':new['rows']['after']['timing'],'parser_candidate_diagnostic_exclusive_stage_seconds':{k:v/1e9 for k,v in new['rows']['after-diagnostic']['exclusive_stage_ns'].items()},'parser_candidate_diagnostic_counts':new['rows']['after-diagnostic']['counts'],'commands':len(commands),'failed_commands':sum(r['returncode']!=0 for r in commands),'timeouts':sum(r['timeout'] for r in commands),'command_wall_seconds':sum(r['seconds'] for r in commands),'full_own_source_run':False,'no_full_suite_ON_POSIX_or_generations':True,'performance_conclusion':'single normal parser pair is slower, no overall speedup claimed; no repeated timing rounds; A11 and BO-G4 remain open'}
    assert result['timeouts']==0
    save(out/'summary.json',result)
    print(json.dumps({k:result[k] for k in ('status','same_input_cases','offline_model_checks','c04','parser_scheduler','commands','failed_commands','timeouts')},ensure_ascii=False))

def main():
    a=argparse.ArgumentParser();a.add_argument('action',choices=['init','run','analyze']);a.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);a.add_argument('--output',type=Path,required=True);a.add_argument('--name');a.add_argument('--input',type=Path);a.add_argument('--limit',type=int,default=30);a.add_argument('--reuse-component',type=Path);a.add_argument('--reuse-manifest',type=Path);v=a.parse_args()
    if v.action=='init':init(v.root.resolve(),v.output.resolve(),v.reuse_component,v.reuse_manifest)
    elif v.action=='run':run(v.root.resolve(),v.output.resolve(),v.name,v.input,v.limit)
    else:analyze(v.root.resolve(),v.output.resolve())
if __name__=='__main__':main()
