"""BO-04 value/call kernels and scale evidence; no production representation change."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,sys,unittest
from audit_bootstrap_phases import command,digest,save,machine,reduce_events

OPS=('read','copy','argument','return','roundtrip','borrow')
def cases():
    rows=[{'kind':'struct','width':w,'depth':1,'results':0,'rounds':512} for w in (1,8,32)]
    rows += [{'kind':'struct','width':4,'depth':d,'results':0,'rounds':512} for d in (2,4)]
    rows += [{'kind':'struct','width':8,'depth':1,'results':r,'rounds':512} for r in (1,2,4)]
    rows += [{'kind':'list','width':n,'depth':1,'results':0,'rounds':512} for n in (1,64,256)]
    rows += [{'kind':'struct','width':8,'depth':1,'results':r,'rounds':n} for r in (0,2) for n in (128,2048)]
    # Complete the same-width 1/2/4 depth series and validate the derived model
    # on combinations that were not used to derive its factors.
    rows += [{'kind':'struct','width':4,'depth':1,'results':0,'rounds':512,'role':'depth-control'},
             {'kind':'struct','width':5,'depth':3,'results':3,'rounds':768,'role':'held-out'},
             {'kind':'struct','width':16,'depth':2,'results':2,'rounds':768,'role':'held-out'}]
    for row in rows:row['id']=f"{row['kind']}-w{row['width']}-d{row['depth']}-r{row['results']}-n{row['rounds']}"
    return rows

def operations(case):return OPS[:-1] if case['results'] else OPS

def source(case):
    width,depth,result_depth,rounds=(case[k] for k in ('width','depth','results','rounds'))
    prefix='import std.time as time\nimport std.gc as gc\nimport std.list as list\n'
    if case['kind']=='list':
        typ='List<int>';value='list.from_slice(['+','.join(str(17+i) for i in range(width))+'])'
        read='unwrap(list.get(inputValue,0))'
    else:
        value=''
        for d in range(1,depth+1):
            fields='\n'.join(f'f{i} '+(f'R{d-1}' if i==0 and d>1 else 'int') for i in range(width))
            prefix+=f'struct R{d}{{{fields}}}\n'
            value=f'R{d}{{'+','.join(f'f{i}:'+(value if i==0 and d>1 else str(17+i)) for i in range(width))+'}'
        typ=f'R{depth}';read='inputValue'+'.f0'*depth
        for _ in range(result_depth):typ=f'Result<{typ}>';value=f'ok({value})'
    prefix+=f'var inputValue {typ}={value}\n'
    prefix+=f'fn take(x {typ}) int{{return 17}}\nfn give() {typ}{{return inputValue}}\nfn echo(x {typ}) {typ}{{return x}}\n'
    if not result_depth:prefix+=f'fn borrow(x mut {typ}) int{{return 17}}\n'
    bodies={'read':f'checksum+={read}\n' if not result_depth else 'if !is_ok(inputValue){print("INVALID")}\nchecksum+=17\n',
            'copy':'let snapshot=inputValue\nchecksum+=17\n',
            'argument':'checksum+=take(inputValue)\n',
            'return':'let snapshot=give()\nchecksum+=17\n',
            'roundtrip':'let snapshot=echo(inputValue)\nchecksum+=17\n',
            'borrow':'checksum+=borrow(inputValue)\n'}
    for op in operations(case):
        prefix+=f'fn kernel_{op}(){{\nvar checksum=0\nlet before=gc.allocated()\nprint("@@hua.phase:begin:{op}")\nlet start=time.monotonic_ns()\nfor i in 0..{rounds}{{\n'+bodies[op]+'}\nlet elapsed=time.monotonic_ns()-start\n'
        prefix+=f'print("@@hua.phase:end:{op}")\nlet allocated=gc.allocated()-before\nprint("ROW","{op}",checksum,elapsed,allocated)\n}}\n'
    first='unwrap('*result_depth+'inputValue'+')'*result_depth+'.f0'*depth if result_depth else read
    prefix+='fn main(){print("PREFLIGHT",'+first+')\n'+''.join(f'kernel_{op}()\n' for op in operations(case))+'}\n'
    return prefix

def parse_output(data,case):
    rows={};preflight=False
    for line in data.decode('utf-8').splitlines():
        if line.startswith('@@hua.phase:'):continue
        if line=='PREFLIGHT 17':preflight=True;continue
        parts=line.split();assert len(parts)==5 and parts[0]=='ROW',line
        _,op,checksum,ns,allocated=parts;assert op in operations(case) and op not in rows,op
        rows[op]={'checksum':int(checksum),'ns':int(ns),'managed_allocations':int(allocated)}
        assert int(checksum)==17*case['rounds'] and int(ns)>0,(case,op)
    assert preflight and set(rows)==set(operations(case)),case
    return rows

def initialize(root,out):
    assert not (out/'manifest.json').exists(),'use fresh evidence directory'
    native=root/'tests/artifacts/build-bo01-symbols-20261016/hua.exe';probe=root/'tests/artifacts/build-bo04-value-profile-20261016/hua_performance_probe.exe'
    frozen={}
    for rel in subprocess.check_output(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=root).split(b'\0'):
        if rel:
            name=rel.decode('utf-8');p=root/name
            if p.is_file():frozen[name]=digest(p)
    tools={'native':native,'probe':probe,'native-kernel':out/'native-kernel.exe','count-kernel':out/'count-kernel.exe'}
    data={'schema':'hua.value-call.audit.v1','git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
          'git_status':subprocess.check_output(['git','-c','core.quotepath=false','status','--short'],cwd=root,text=True,encoding='utf-8').splitlines(),
          'host':machine(),'files':frozen,'tools':{k:{'path':str(p),'sha256':digest(p)} for k,p in tools.items()},'cases':cases(),
          'contract':{'vm_steps':500000000,'calls':128,'gc_interval':65536,'diagnostic_command_seconds':30,'full_own_source_run':False},
          'plan':{'dimensions':'vary one of width, chain depth, Result wrapper depth or transfers; flat scalar slot/native integer controls are not a full replacement',
                  'native_times':'ordinary library, batches <=128 and aggregate timed body >=20 ms when attainable, fresh heap each batch; diagnostic counts timed separately',
                  'hua_times':'within-kernel monotonic ns; same checksums through AST/source VM/source-free HUAB; production and diagnostic differ only hooks',
                  'stop':'first failing command saves evidence; diagnose or change a bounded plan before continuing; never overwrite prior command outputs'}}
    inputs=out/'inputs';inputs.mkdir()
    for case in data['cases']:
        path=inputs/(case['id']+'.hua');path.write_text(source(case),encoding='utf-8',newline='\n');case['source_sha256']=digest(path)
    save(out/'manifest.json',data)

def verify(root,out):
    m=json.loads((out/'manifest.json').read_text(encoding='utf-8'))
    for t in m['tools'].values():assert digest(t['path'])==t['sha256'],t
    for name,sha in m['files'].items():
        if name.startswith(('src/','include/','selfhost/')):assert digest(root/name)==sha,name
    for case in m['cases']:assert digest(out/'inputs'/(case['id']+'.hua'))==case['source_sha256']
    return m

def native_matrix(root,out,one=None):
    m=verify(root,out);target=out/'native';target.mkdir(exist_ok=True);rows=[]
    destination=target/((one+'.json') if one else 'results.json')
    assert not destination.exists(),'completed native evidence is immutable'
    try:
        for case in m['cases']:
            if one and case['id']!=one:continue
            pair={}
            for kind in ('native-kernel','count-kernel'):
                _,stdout,_=command(root,out,case['id']+'-'+kind,[Path(m['tools'][kind]['path']),case['kind'],case['width'],case['depth'],case['results'],case['rounds']])
                results=[json.loads(line) for line in stdout.decode().splitlines()]
                for row in results:
                    assert row['checksum']==17*row['operations'],row
                    if row['counts'] is not None:
                        c=row['counts'];assert c['copy_struct_fields']==case['width']*c['copy_struct']
                        assert c['gc_collections']==0 and row['collected_objects']==0,(case,row)
                        if row['operation']=='copy':
                            assert c['copy_struct']==(case['depth']*case['rounds'] if case['kind']=='struct' else 0)
                            assert c['copy_result']==case['results']*case['rounds']
                pair[kind]=results
            assert {x['operation'] for x in pair['native-kernel']}=={x['operation'] for x in pair['count-kernel']}
            for normal in pair['native-kernel']:
                counted=next(x for x in pair['count-kernel'] if x['operation']==normal['operation'])
                assert normal['managed_allocations']*counted['operations']==counted['managed_allocations']*normal['operations']
            rows.append({'case':case,**pair});save(destination,{'status':'running','rows':rows})
    except BaseException as error:
        save(destination,{'status':'stopped','error':repr(error),'rows':rows});raise
    assert rows,'unknown native case'
    save(destination,{'status':'passed','rows':rows})

def hua_case(root,out,case,m):
    target=out/'hua';target.mkdir(exist_ok=True);result=target/(case['id']+'.json')
    assert not result.exists(), 'completed evidence may only be reused by the hash-reviewed matrix runner'
    native=Path(m['tools']['native']['path']);probe=Path(m['tools']['probe']['path'])
    src=out/'inputs'/(case['id']+'.hua');artifact=target/(case['id']+'.huab');label=case['id']
    build,_,_=command(root,out,label+'-build',[native,'build',src,'-o',artifact])
    ast,astout,_=command(root,out,label+'-ast',[native,'interpret',src,'--execution-profile','compiler'])
    astrows=parse_output(astout,case)
    source_vm,sourceout,_=command(root,out,label+'-source-vm',[native,'run',src,'--execution-profile','compiler','--report-budget'])
    sourcerows=parse_output(sourceout,case)
    hidden=src.with_suffix('.hidden');src.rename(hidden)
    try:
        normal,stdout,stderr=command(root,out,label+'-huab',[native,'run',artifact,'--execution-profile','compiler','--report-budget'])
        normalrows=parse_output(stdout,case);budget=json.loads(stderr)
        events=target/(label+'-events.jsonl')
        diagnostic,diagout,diagerr=command(root,out,label+'-counts',[probe,'--phase-events',events,artifact,'bo04-kernel'])
        diagnosticrows=parse_output(diagout,case)
    finally:hidden.rename(src)
    trace=[json.loads(line) for line in events.read_text(encoding='utf-8').splitlines()];phases=reduce_events(trace)
    metrics=json.loads(diagerr.decode().splitlines()[-1]);assert metrics['steps']==budget['steps'] and metrics['peak_calls']==budget['peak_calls']
    rows=[]
    for op in operations(case):
        assert astrows[op]['checksum']==sourcerows[op]['checksum']==normalrows[op]['checksum']==diagnosticrows[op]['checksum']
        assert normalrows[op]['managed_allocations']==diagnosticrows[op]['managed_allocations']
        counts=phases[op]['counters']
        assert counts['copy_struct_fields']==case['width']*counts['copy_struct'],(case,op)
        if case['kind']=='list':assert counts['copy_struct']==0
        rows.append({'operation':op,'kernel':normalrows[op],'counts':counts,'histogram':phases[op]['struct_copies'],
                     'diagnostic_ns':phases[op]['ns'],'diagnostic_cpu_ns':phases[op]['cpu_ns']})
    data={'status':'passed','case':case,'rows':rows,'artifact_sha256':digest(artifact),'budget':budget,'source_hidden_for_huab':True,
          'commands':{'build':build,'ast':ast,'source_vm':source_vm,'normal':normal,'diagnostic':diagnostic}}
    save(result,data);print(json.dumps({'case':label,'rows':len(rows),'status':'passed'}),flush=True)
    return data

def hua_matrix(root,out,one=None):
    m=verify(root,out);rows=[];reused=[]
    destination=out/(('case-'+one+'-results.json') if one else 'hua-results.json')
    for case in m['cases']:
        if one and case['id']!=one:continue
        result=out/'hua'/(case['id']+'.json')
        if result.exists():
            data=json.loads(result.read_text(encoding='utf-8'));assert data['status']=='passed' and data['case']==case
            assert digest(out/'hua'/(case['id']+'.huab'))==data['artifact_sha256']
            for row in data['commands'].values():assert not row['timeout'] and row['returncode']==0
            reused.append({'case':case['id'],'result_sha256':digest(result),'artifact_sha256':data['artifact_sha256']})
        else:data=hua_case(root,out,case,m)
        rows.append(data)
        save(destination,{'status':'running','rows':rows,'reused_verified':reused})
    assert rows,'unknown case'
    save(destination,{'status':'passed','rows':rows,'reused_verified':reused})

def semantics(root,out):
    m=verify(root,out);native=Path(m['tools']['native']['path']);target=out/'semantic';target.mkdir();rows=[]
    wanted={'ordinary_struct_parameter_copy','return_struct_snapshot_before_defer','return_fixed_array_snapshot_before_defer',
            'return_result_struct_snapshot_before_defer','multi_rhs_struct_snapshot_after_all_expressions','defer_arguments_snapshot_after_all_expressions','escaped_closure_list_shared_readonly_view'}
    cases=json.loads((root/'tests/data/a04_execution_cases.json').read_text(encoding='utf-8'))
    for case in cases:
        if case['name'] not in wanted:continue
        assert case['phase']=='ok';source=target/(case['name']+'.hua');source.write_text(case['source'],encoding='utf-8',newline='\n');artifact=source.with_suffix('.huab')
        for mode in ('interpret','run','build'):
            args=[native,mode,source]+(['-o',artifact] if mode=='build' else [])
            _,stdout,_=command(root,out,'semantic-'+case['name']+'-'+mode,args)
            if mode!='build':assert stdout.decode('utf-8').replace('\r\n','\n')==case['want'],case['name']
        source.rename(source.with_suffix('.hidden'))
        _,stdout,_=command(root,out,'semantic-'+case['name']+'-huab',[native,'run',artifact])
        assert stdout.decode('utf-8').replace('\r\n','\n')==case['want'],case['name']
        rows.append({'case':case['name'],'paths':3,'source_hidden_for_huab':True,'artifact_sha256':digest(artifact)})
    assert len(rows)==len(wanted);save(target/'results.json',{'status':'passed','rows':rows})

def real_inputs(root,out):
    m=verify(root,out);native=Path(m['tools']['native']['path']);probe=Path(m['tools']['probe']['path'])
    previous=root/'tests/artifacts/build/bo01-native-cpu-r2-20261016';old=json.loads((previous/'lookup-candidate/results.json').read_text(encoding='utf-8'))
    assert old['runtime_sha256']==m['tools']['native']['sha256'];component=previous/'compiler.huab'
    assert digest(component)==json.loads((previous/'manifest.json').read_text(encoding='utf-8'))['component_sha256']
    target=out/'real';target.mkdir();rows=[]
    for prior in old['rows']:
        name=prior['case'];source=previous/'inputs'/(name+'.hua');a=target/(name+'.huab');b=target/(name+'-counts.huab')
        normal,report,stderr=command(root,out,'real-'+name+'-normal',[native,'run',component,'--execution-profile','compiler','--report-budget','--',source,a])
        events=target/(name+'-events.jsonl')
        diagnostic,diagout,diagerr=command(root,out,'real-'+name+'-counts',[probe,'--phase-events',events,component,source,b])
        assert report==diagout and a.read_bytes()==b.read_bytes() and digest(a)==prior['artifact_sha256']
        assert hashlib.sha256(report).hexdigest()==prior['report_sha256']
        trace=[json.loads(line) for line in events.read_text(encoding='utf-8').splitlines()]
        phase=reduce_events(trace)['execution'];budget=json.loads(stderr);metrics=json.loads(diagerr.decode().splitlines()[-1])
        assert budget['steps']==metrics['steps'] and budget['peak_calls']==metrics['peak_calls']
        rows.append({'case':name,'input_sha256':digest(source),'component_sha256':digest(component),'normal':normal,'diagnostic':diagnostic,
                     'artifact_sha256':digest(a),'report_sha256':hashlib.sha256(report).hexdigest(),'budget':budget,
                     'counts':phase['counters'],'histogram':phase['struct_copies']})
        save(target/'results.json',{'status':'running','rows':rows})
    save(target/'results.json',{'status':'passed','rows':rows})

def analyze(root,out):
    m=verify(root,out);native_rows=[];hua_rows=[];model_checks=0
    for file in (out/'native').glob('*.json'):
        part=json.loads(file.read_text(encoding='utf-8'));assert part['status']=='passed';native_rows+=part['rows']
    native={x['case']['id']:x for x in native_rows};assert len(native)==len(m['cases'])
    for case in m['cases']:
        row=json.loads((out/'hua'/(case['id']+'.json')).read_text(encoding='utf-8'));assert row['status']=='passed';hua_rows.append(row)
        source_budget=json.loads((out/'commands'/(case['id']+'-source-vm.stderr')).read_text(encoding='utf-8'))
        assert source_budget==row['budget'],(case,'source/persisted budget');model_checks+=1
        srcrows=parse_output((out/'commands'/(case['id']+'-source-vm.stdout')).read_bytes(),case)
        assert all(srcrows[x['operation']]['managed_allocations']==x['kernel']['managed_allocations'] for x in row['rows'])
        model_checks+=1
        width,depth,r,n=(case[k] for k in ('width','depth','results','rounds'))
        ns={'read':0,'copy':depth,'move-shared':depth,'move-unique':0,'enforce':depth*r,'borrow':depth*r,
            'argument':depth*(r+1),'return':depth*(r+1),'roundtrip':2*depth*(r+1),'slots':0,'cpp-vector':0}
        nr={'read':0,'copy':r,'move-shared':r,'move-unique':0,'enforce':r*(r-1)//2,'borrow':r*(r-1)//2,
            'argument':r*(r+1)//2,'return':r*(r+1)//2,'roundtrip':r*(r+1),'slots':0,'cpp-vector':0}
        na={'read':0,'copy':depth+r,'move-shared':depth+r,'move-unique':0,'enforce':depth*r+r*(r+1)//2,
            'borrow':depth*r+r*(r+1)//2,'argument':depth*(r+1)+r*(r+3)//2,'return':depth*(r+1)+r*(r+3)//2,
            'roundtrip':2*depth*(r+1)+r*(r+3),'slots':0,'cpp-vector':0}
        for counted in native[case['id']]['count-kernel']:
            op=counted['operation'];counts=counted['counts'];factor=case['kind']=='struct'
            assert counts['copy_struct']==n*ns[op]*factor,(case,op,'native Struct')
            assert counts['copy_result']==n*nr[op]*factor,(case,op,'native Result')
            assert counted['managed_allocations']==n*na[op]*factor,(case,op,'native allocation')
            model_checks+=3
            if op=='move-unique':
                assert counts['copy_move']==n*(depth*width+r+1),(case,'unique move traversal');model_checks+=1
        hs={'read':0,'copy':depth*max(1,r),'argument':depth*(r+1),'return':depth*(2*r+1),'roundtrip':depth*(3*r+2),'borrow':0}
        hr={'read':0,'copy':r*(r-1)//2,'argument':r*(r+1)//2,'return':r*r,'roundtrip':r*(3*r+1)//2,'borrow':0}
        for measured in row['rows']:
            op=measured['operation'];counts=measured['counts'];factor=case['kind']=='struct'
            assert counts['copy_struct']==n*hs[op]*factor,(case,op,'Hua Struct')
            assert counts['copy_struct_fields']==n*width*hs[op]*factor,(case,op,'Hua fields')
            if factor:assert counts['copy_result']==n*hr[op],(case,op,'Hua Result');model_checks+=1
            model_checks+=2
    byid={row['case']['id']:row for row in hua_rows};linear=[]
    for r in (0,2):
        group=[byid[f'struct-w8-d1-r{r}-n{n}'] for n in (128,512,2048)]
        for op in operations(group[0]['case']):
            metrics=[]
            for metric in ('copy_struct','copy_struct_fields','copy_result','enforce_type_calls','vm_steps','user_calls','managed_allocations'):
                values=[next(v for v in x['rows'] if v['operation']==op)['counts'][metric] for x in group]
                assert (values[1]-values[0])*1536==(values[2]-values[1])*384,(r,op,metric,values)
                slope=(values[1]-values[0])/384;offset=values[0]-128*slope
                metrics.append({'metric':metric,'per_transfer':slope,'fixed':offset});model_checks+=1
            linear.append({'results':r,'operation':op,'metrics':metrics})
    width_steps={}
    for op in OPS:
        values=[next(x for x in byid[f'struct-w{w}-d1-r0-n512']['rows'] if x['operation']==op)['counts']['vm_steps'] for w in (1,8,32)]
        assert len(set(values))==1,(op,'width changed VM instruction count',values)
        width_steps[op]=values[0];model_checks+=1
    real=json.loads((out/'real/results.json').read_text(encoding='utf-8'));assert real['status']=='passed';real_summary=[]
    for row in real['rows']:
        counts=row['counts'];hist=row['histogram'];field_total=sum(v['fields'] for v in hist.values());assert field_total==counts['copy_struct_fields']
        real_summary.append({'case':row['case'],'copy_struct':counts['copy_struct'],'copy_fields':field_total,'copy_result':counts['copy_result'],
                             'enforce_type_calls':counts['enforce_type_calls'],'gc_collections':counts['gc_collections'],'diagnostic_gc_ns':counts['gc_nanoseconds'],
                             'normal_seconds':row['normal']['seconds'],'normal_cpu_seconds':row['normal']['cpu_seconds'],
                             'top_structs':[{'identity':k,**v,'field_fraction':v['fields']/field_total} for k,v in sorted(hist.items(),key=lambda kv:kv[1]['fields'],reverse=True)[:10]]})
    commands=[json.loads(p.read_text(encoding='utf-8')) for p in (out/'commands').glob('*.json')]
    assert all(not c['timeout'] and c['returncode']==0 for c in commands)
    result={'status':'passed','cases':len(m['cases']),'hua_operations':sum(len(r['rows']) for r in hua_rows),
            'native_operations_per_build':sum(len(r['native-kernel']) for r in native_rows),'model_checks':model_checks,
            'held_out':[r['case'] for r in hua_rows if r['case'].get('role')=='held-out'],'linear_work':linear,'width_vm_steps':width_steps,
            'real_inputs':real_summary,'command_count':len(commands),'command_wall_seconds':sum(c['seconds'] for c in commands),'command_timeouts':0,
            'full_own_source_run':False,'production_changed':False,'bounds':'model predicts object/field work within tested acyclic shapes; not total CPU, bytes, full compiler or all Result paths',
            'recommendation':'Review separation of Result validation from payload materialization before any implementation; retain layout slot prototype as scalar-only alternative requiring full compatibility design'}
    save(out/'summary.json',result);save(out/'work-model.json',{'status':'passed','checks':model_checks,'native':native_rows,'hua':hua_rows})
    print(json.dumps({k:result[k] for k in ('status','cases','hua_operations','native_operations_per_build','model_checks','command_count','command_timeouts','full_own_source_run')}),flush=True)


class Tests(unittest.TestCase):
    def test_plan_independent_axes(self):
        rows=cases();self.assertEqual(len(rows),18);self.assertEqual(len({c['id'] for c in rows}),18)
    def test_result_fixture_not_borrow_substitution(self):
        case=next(c for c in cases() if c['results']==4);text=source(case)
        self.assertIn('Result<Result<Result<Result<R1>>>>',text);self.assertNotIn('fn borrow(',text)
        self.assertIn('unwrap(unwrap(unwrap(unwrap(inputValue)))).f0',text)
    def test_list_length_outside_loop(self):
        case=next(c for c in cases() if c['kind']=='list' and c['width']==256);text=source(case)
        self.assertEqual(text.count('list.from_slice'),1);self.assertIn('fn borrow(x mut List<int>)',text)
    def test_partial_and_invalid_outputs_rejected(self):
        case=cases()[0]
        with self.assertRaises(AssertionError):parse_output(b'PREFLIGHT 17\n',case)
        data=b'PREFLIGHT 17\r\n'+b''.join(f'ROW {op} {17*512} 123 512\r\n'.encode() for op in operations(case))
        self.assertEqual(len(parse_output(data,case)),6)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('self-test','init','native','hua','semantic','real','analyze'))
    parser.add_argument('--output',type=Path);parser.add_argument('--case');args=parser.parse_args()
    if args.mode=='self-test':return 0 if unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests)).wasSuccessful() else 1
    root=Path(__file__).resolve().parents[2];out=args.output.resolve();assert out.is_relative_to(root/'tests/artifacts') and out!=root/'tests/artifacts'
    try:
        if args.mode=='init':initialize(root,out)
        elif args.mode=='native':native_matrix(root,out,args.case)
        elif args.mode=='hua':hua_matrix(root,out,args.case)
        elif args.mode=='semantic':semantics(root,out)
        elif args.mode=='real':real_inputs(root,out)
        elif args.mode=='analyze':analyze(root,out)
    except BaseException as error:
        failure=out/(args.mode+'-failure.json')
        if not failure.exists():save(failure,{'status':'stopped','error':repr(error),'retry':False})
        raise
    return 0
if __name__=='__main__':raise SystemExit(main())
