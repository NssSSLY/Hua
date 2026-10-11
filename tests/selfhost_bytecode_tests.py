# C06: exact instructions, four compiler paths, independent behavior and corruptions.
from pathlib import Path
import ast,subprocess,sys,json,struct,shutil,re,importlib.util,copy,io,time
from unittest.mock import patch
from contextlib import redirect_stderr
hua,probe,oracle,core_driver,driver,repo,build=map(lambda p:Path(p).resolve(),sys.argv[1:8])
smoke=len(sys.argv)>8 and sys.argv[8]=='--smoke'
out=build/'c06';out.mkdir(exist_ok=True);work=out/'acceptance';work.mkdir(exist_ok=True)
rows=[];checks=0;started=time.monotonic()
def invoke(args,code=0,timeout=600):
    p=subprocess.run(list(map(str,args)),capture_output=True,text=True,encoding='utf-8',timeout=timeout)
    assert p.returncode==code,(args,p.returncode,p.stdout[:1500],p.stderr[:4000])
    return p
spec=importlib.util.spec_from_file_location('c06_runner',repo/'tools/hua_frontend.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
contract=json.loads((repo/'selfhost/contract.json').read_text(encoding='utf-8'))
entry=repo/'selfhost/bytecode_main.hua'
paths=[(hua,'run',entry)]
persisted=work/'persisted';persisted.mkdir(exist_ok=True)
if not smoke:
    for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,persisted/p.name)
    invoke([probe,'check',entry])
    artifact=persisted/'bytecode.huab'
    invoke([probe,'build',persisted/'bytecode_main.hua','-o',artifact])
    assert struct.unpack_from('<II',artifact.read_bytes(),8)==(7,1);checks+=2
    assert persisted.resolve().parent==work.resolve()
    for p in persisted.glob('*.hua'):p.unlink()
    assert not list(persisted.glob('*.hua'));checks+=1
    paths=[(hua,'interpret',entry),(hua,'run',entry),(hua,'run',artifact),(probe,'run',entry)]
fixtures={}
for n in ast.parse((repo/'tests/selfhost_lowering_tests.py').read_text(encoding='utf-8')).body:
    if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name) and n.targets[0].id in ('positive','runtime_errors','negative'):
        fixtures[n.targets[0].id]=ast.literal_eval(n.value)
positive=fixtures['positive'];errors={k:(v if isinstance(v,str) else v[0]) for k,v in fixtures['runtime_errors'].items()};negative=fixtures['negative']
negative['array_mixed_spelling']=('fn f(x [02]int) int{return x[1]}\nfn main(){let a [2]int=[1,2]\nprint(f(a))}\n','E3004')
positive.update({
'array_raw_argument':('fn f(x [0_2]int) int{return x[1]}\nfn main(){print(f([1,2]))}\n','2\n'),
'array_raw_metadata':('fn f(x [0_2]int) int{return x[1]}\nfn g(x [02]int) int{return x[1]}\nfn main(){print("raw")}\n','raw\n'),
'map_array_raw':('fn main(){let a map[string][0_2]int=map[string][0_2]int{"x":[1,2]}\nprint(type(a),type(unwrap(a["x"])),unwrap(a["x"])[1])}\n','map<string,[0_2]int> [2]int 2\n'),
'literal_bases':('fn main(){print(0x7f,0b101,0o17,1_234,-0x8000000000000000,+1,-(-1))}\n',None),
'float_constants':('fn main(){print(0.1,1.2345678901234567,1e3,-0.0,+(-1.5),01.5,~2)}\n',None),
'multi_discard':('fn f()(int,string){return 2,"a"}\nfn main(){let _,b=f()\nvar a,_=f()\na,_=f()\nprint(a,b)}\n','2 a\n'),
'nested_loops':('fn main(){var s=0\nfor i in 0..3{var j=0\nwhile j<4{j+=1\nif j==2{continue}\nif i==1{break}\ns+=j}}\nprint(s)}\n','16\n'),
'builtin_catalog':('fn main(){print(min(2,1),max(2,1),abs(-2),clamp(7,0,3),is_some(1),is_none(nil))}\n','1 2 2 3 true true\n'),
'input_arguments':('import std.os as o\nfn main(){print(o.args())}\n',None),
'top_initialization':('var n=1\nprint("init",n)\nfn main(){print("main",n)}\n','init 1\nmain 1\n')
})
errors.update({

'array_raw_binding':'fn f(x [02]int) int{return x[1]}\nfn main(){let a [02]int=[1,2]\nprint(f(a))}\n',
'int_too_large':'fn main(){print(9223372036854775808)}\n',
'min_negation':'fn main(){print(-(-9223372036854775808))}\n',
'float_too_large':'fn main(){print(1e999)}\n',
'float_too_small':'fn main(){print(1e-999)}\n'
})
def report(source):
    result=[]
    for exe,action,target in paths:
        p=invoke([exe,action,target,'--execution-profile','compiler','--report-budget','--',source])
        r=json.loads(p.stdout);metrics=json.loads(p.stderr)
        assert metrics['profile']=='compiler' and 0<metrics['steps']<=500000000 and 0<metrics['peak_calls']<=128
        result.append(r)
    assert all(x==result[0] for x in result)
    return result[0]
def projected(r):
    def code(c):
        result=copy.deepcopy(c)
        for op in result['instructions']:
            s=r['sites'][op.pop('site')]
            op.update({k:s[k] for k in ('module','start','end','line','column')})
            if op['constant']['kind']==3:op['constant']['text']=struct.pack('<d',float(op['constant']['text'])).hex()
        return result
    return {'initializer':code(r['initializer']),'functions':sorted([code(f['code']) for f in r['functions']],key=lambda c:c['name'])}
def reference(r):
    for c in [r['initializer']]+r['functions']:
        for op in c['instructions']:
            if op['constant']['kind']==3:op['constant']['text']=struct.pack('<d',float(op['constant']['text'])).hex()
    return r
def case(label,text,expected=None,error=None,files=None,filename='input.hua'):
    global checks
    project=work/label;project.mkdir(exist_ok=True);source=project/filename;source.write_text(text,encoding='utf-8',newline='')
    for n,body in (files or {}).items():
        p=project/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body,encoding='utf-8',newline='\n')
    marker=project/'effect.txt'
    if label=='filesystem_effect':
        marker.unlink(missing_ok=True)
        source.write_text('import std.fs as f\nlet done=f.write_text('+json.dumps(marker.as_posix())+',"touched")\nfn main(){print(unwrap(done))}\n',encoding='utf-8')
    r=report(source)
    if label=='filesystem_effect':assert not marker.exists(),'emitter executed input'
    if error:
        assert r['status']=='error' and r['error'].startswith(error+'@'),(label,r)
        assert not any(k in r for k in ('initializer','functions','sites','sources','verification'))
        checks+=6;rows.append({'case':label,'status':'rejected','error':r['error']});return r
    assert r['status']=='ok',(label,r)
    runner.validate_bytecode(r,contract)
    invoke([hua,'check',source])
    runtime=label in errors;args=['--','hello','世界'] if label=='input_arguments' else []
    baseline=[invoke([hua,action,source,*args],1 if runtime else 0) for action in ('interpret','run')]
    assert baseline[0].stdout==baseline[1].stdout,(label,baseline)
    if expected is not None:assert baseline[0].stdout==expected,(label,baseline[0].stdout,expected)
    core=json.loads(invoke([hua,'run',repo/'selfhost/lowering_main.hua','--execution-profile','compiler','--',source]).stdout)
    cp=project/'core.json';cp.write_text(json.dumps(core,ensure_ascii=False),encoding='utf-8')
    host=json.loads(invoke([core_driver,'image',cp]).stdout)
    actual=projected(r);wanted=reference(host)
    if actual!=wanted:
        (project/'wanted.json').write_text(json.dumps(wanted,ensure_ascii=False,indent=2),encoding='utf-8')
        (project/'actual.json').write_text(json.dumps(actual,ensure_ascii=False,indent=2),encoding='utf-8')
    assert actual==wanted,(label,'instruction mismatch',project)
    target=project/'image.json';target.write_text(json.dumps(r,ensure_ascii=False),encoding='utf-8')
    invoke([driver,'verify',target]);assert reference(json.loads(invoke([driver,'image',target]).stdout))==wanted
    artifact=project/'image.huab';invoke([driver,'build',target,'-o',artifact])
    assert struct.unpack_from('<II',artifact.read_bytes(),8)[0] in (6,7)
    assert project.resolve().parent==work.resolve()
    for p in project.rglob('*.hua'):p.unlink()
    for exe,action,target in [(driver,'run',target),(hua,'run',artifact)]:
        p=invoke([exe,action,target,*args],1 if runtime else 0)
        assert p.stdout==baseline[0].stdout,(label,p.stdout,baseline[0].stdout)
        if runtime:
            ec=re.search(r'error\[(E\d+)\]',baseline[0].stderr)[1]
            assert 'error['+ec+']' in p.stderr,(label,ec,p.stderr)
            pos=re.search(r':(\d+):(\d+)',baseline[0].stderr)
            assert f'{filename}:{pos[1]}:{pos[2]}' in p.stderr,(label,baseline[0].stderr,p.stderr)
    checks+=14+r['instructions'];rows.append({'case':label,'status':'ok','instructions':r['instructions'],'output':baseline[0].stdout})
    return r
case('filesystem_effect','',expected='true\n')
for n,(text,expected) in positive.items():case(n,text,expected)
for n,text in errors.items():case(n,text)
for n,(text,ec) in negative.items():case(n,text,error=ec)
case('modules','import dep as d\nfn main(){print(d.f(3))}\n','dep\n4\n',files={'dep.hua':'print("dep")\npub fn f(x int) int{return x+1}\n'})
case('module_enum','import dep as d\nfn main(){print(d.f(d.E.A(3)))}\n','3\n',files={'dep.hua':'pub enum E{A(int),B}\npub fn f(x E) int{match x{A(v)=>{return v}\nB=>{return 0}}}\n'})
case('module_suffix','import model as a\nimport pkg.model as b\nfn main(){print(type(a.A{x:1}),type(b.B{x:2}))}\n','A B\n',files={'model.hua':'pub struct A{x int}\n','pkg/model.hua':'pub struct B{x int}\n'})
case('special_filename','struct A{x int}\nfn main(){print(type(A{x:1}))}\n','A\n',filename='带 #空格$.hua')
# Exercise Hua verifier independently with damaged typed images.
v=work/'verifier';v.mkdir(exist_ok=True)
for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,v/p.name)
source=r'''
import bytecode as b
import bytecode_verify as v
import std.list as l
fn op(k int, target int, argument int, text string, t string, names []string, flags []bool) b.Instruction {
    return b.Instruction{op:k,site:0,text:text,type_name:t,argument:argument,target:target,flag:false,constant:b.Scalar{kind:0,text:""},names:names,contextual:flags}
}
fn structures() List<b.Structure>{var a []b.Structure=[]
return l.from_slice(a)}
fn functions() List<b.Function>{var a []b.Function=[]
return l.from_slice(a)}
fn test(label string, instructions []b.Instruction) {
    let code=b.Code{name:"<initialize>",instructions:l.from_slice(instructions)}
    let image=b.Image{entry:"input.hua",sites:l.from_slice([b.Site{node:0,module:"input.hua",origin:0,start:0,end:0,line:1,column:1,role:"source"}]),structures:structures(),functions:functions(),initializer:code,instructions:len(instructions),work:len(instructions)}
    let result=v.verify(image)
    print(label,is_err(result),unwrap_or(result,v.Verification{instructions:0,reachable:0,slots:0,work:0,max_stack:0,max_scopes:0,max_iterators:0}).reachable)
}
fn main(){
@CASES@
}
'''
bad_cases={
'empty_array_size':[(0,0,0,'','[___]int',[],[]),(22,0,0,'','',[],[])],
'underflow':[(3,0,0,'','',[],[]),(0,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'scope_underflow':[(10,0,0,'','',[],[]),(0,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'unwind_underflow':[(11,0,1,'','',[],[]),(0,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'iterator_underflow':[(25,1,0,'','',['i'],[]),(0,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'iterator_end_underflow':[(26,0,0,'','',[],[]),(0,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'bad_jump':[(6,99,0,'','',[],[]),(22,0,0,'','',[],[])],
'bad_opcode':[(41,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'falls_off':[(0,0,0,'','',[],[]),(3,0,0,'','',[],[])],
'return_extra':[(0,0,0,'','',[],[]),(0,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'unknown_binding':[(1,0,0,'missing','',[],[]),(22,0,0,'','',[],[])],
'unknown_struct':[(19,0,0,'Missing','',[],[]),(22,0,0,'','',[],[])],
'bad_type':[(0,0,0,'','Missing',[],[]),(22,0,0,'','',[],[])],
'bad_call_flags':[(0,0,0,'','',[],[]),(21,0,2,'','',[],[]),(22,0,0,'','',[],[])],
'bad_propagation':[(0,0,0,'','',[],[]),(37,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'bad_map':[(32,0,0,'','map<float,int>',[],[]),(22,0,0,'','',[],[])],
'bad_bind':[(0,0,0,'','',[],[]),(2,0,0,'x','int',[],[]),(0,0,0,'','',[],[]),(22,0,0,'','',[],[])],
'unreachable_bad':[(0,0,0,'','',[],[]),(22,0,0,'','',[],[]),(6,99,0,'','',[],[])],
'location_kind':[(0,0,0,'','',[],[]),(13,0,0,'x','',[],[]),(22,0,0,'','',[],[])],
'branch_merge':[(0,0,0,'','',[],[]),(7,4,0,'','',[],[]),(9,0,0,'','',[],[]),(6,5,0,'','',[],[]),(0,0,0,'','',[],[]),(22,0,0,'','',[],[])]
}
def hx(x):
    if isinstance(x,str):return json.dumps(x,ensure_ascii=False)
    if isinstance(x,list):return '['+','.join(hx(y) for y in x)+']'
    return str(x)
lines=['test('+hx(n)+',['+','.join('op('+','.join(hx(x) for x in op)+')' for op in ops)+'])' for n,ops in bad_cases.items()]
vp=v/'verify_main.hua';vp.write_text(source.replace('@CASES@','\n'.join(lines)),encoding='utf-8')
for action in ('run',) if smoke else ('interpret','run'):
    lines=invoke([hua,action,vp,'--execution-profile','compiler']).stdout.splitlines()
    assert len(lines)==len(bad_cases) and all(line.split()[1]=='true' for line in lines),(action,lines)
    checks+=len(lines)
rows.append({'case':'Hua verifier corruptions','status':'rejected','count':len(bad_cases)})
project=work/'protocol';project.mkdir(exist_ok=True);src=project/'input.hua';src.write_text('fn main(){print(2)}\n',encoding='utf-8')
good=report(src);target=project/'output.json';mutations=[]
def bad(label,edit):
    r=copy.deepcopy(good);edit(r);mutations.append((label,r))
bad('op',lambda r:r['functions'][0]['code']['instructions'][0].update(op=99))
bad('jump',lambda r:r['functions'][0]['code']['instructions'][0].update(op=6,target=999))
bad('count',lambda r:r.update(instructions=1000001))
bad('site',lambda r:r['sites'][0].update(line=99))
bad('origin',lambda r:r['sites'][0].update(origin=-5))
bad('constant',lambda r:next(x for x in r['functions'][0]['code']['instructions'] if x['op']==0)['constant'].update(kind=2,text='9223372036854775808'))
bad('function',lambda r:r['functions'][0].update(name='other'))
bad('parameter',lambda r:r['functions'][0].update(parameters=[{'name':'x','type_name':'int','writable':False,'site':0}]))
bad('verification',lambda r:r['verification'].update(max_stack=4097))
bad('reference',lambda r:next(x for x in r['functions'][0]['code']['instructions'] if x['op']==1).update(text='missing'))
bad('call_flags',lambda r:next(x for x in r['functions'][0]['code']['instructions'] if x['op']==21).update(contextual=[]))
bad('empty_array_size',lambda r:r['functions'][0].update(returns=['[___]int']))
bad('profile',lambda r:r.update(profile='C05-L1'))
bad('unknown_std',lambda r:next(x for x in r['functions'][0]['code']['instructions'] if x['op']==1).update(text='$core$$std$strings$missing'))
bad('bool_opcode',lambda r:r['functions'][0]['code']['instructions'][0].update(op=True))
bad('failure_partial',lambda r:r.update(status='error',stage='bytecode',error='E6002@1:1 invalid'))
for label,r in mutations:
    target.write_text('KEEP',encoding='utf-8')
    fake=subprocess.CompletedProcess([],0,json.dumps(r),'')
    with patch.object(runner.subprocess,'run',return_value=fake),redirect_stderr(io.StringIO()) as err:
        rc=runner.main(['emit',str(src),'-o',str(target),'--hua',str(hua)])
    assert rc==3 and target.read_text()=='KEEP',(label,rc,err.getvalue())
    checks+=2

for label,side_effect,stdout,stderr,expected in [
    ('timeout',subprocess.TimeoutExpired('hua',600),'','',1),
    ('host_budget',None,'','error[E4099]: budget',1),
    ('host_internal',None,'','error[E4007]: broken',3),
    ('report_budget',None,'x'*16777217,'',3)]:
    target.write_text('KEEP',encoding='utf-8')
    kwargs={'side_effect':side_effect} if side_effect else {'return_value':subprocess.CompletedProcess([],1 if stderr else 0,stdout,stderr)}
    with patch.object(runner.subprocess,'run',**kwargs),redirect_stderr(io.StringIO()):
        rc=runner.main(['emit',str(src),'-o',str(target),'--hua',str(hua)])
    assert rc==expected and target.read_text()=='KEEP',(label,rc)
    checks+=2
# Transport source normalization/relocation is deterministic, including BOM+CRLF.
copies=[]
for label,text in [('plain','fn main(){print(1)}\n'),('bom_crlf','\ufefffn main(){print(1)}\r\n')]:
    directory=work/('determinism_'+label);directory.mkdir(exist_ok=True);source_=directory/'same.hua';source_.write_text(text,encoding='utf-8',newline='')
    copies.append(report(source_))
assert copies[0]==copies[1];checks+=2
rows.append({'case':'normalized relocation determinism','status':'ok'})
# Encoding accepts scalar capacity, then refuses oversized complete JSON without a partial artifact.
capacity=v/'capacity_main.hua'
capacity.write_text(r"""
import bytecode as b
import bytecode_verify as v
import bytecode_report as r
import types as t
import std.list as l
import std.os as o
fn main(){
 let m=unwrap(t.analyze(o.args()[0]))
 var text="x"
 for i in 0..24{text+=text}
 var ops []b.Instruction=[]
 var code=l.from_slice(ops)
 unwrap(l.append(code,b.Instruction{op:0,site:0,text:"",type_name:"",argument:0,target:0,flag:false,constant:b.Scalar{kind:4,text:text},names:[],contextual:[]}))
 unwrap(l.append(code,b.Instruction{op:22,site:0,text:"",type_name:"",argument:0,target:0,flag:false,constant:b.Scalar{kind:0,text:""},names:[],contextual:[]}))
 var structures []b.Structure=[]
 var functions []b.Function=[]
 let image=b.Image{entry:"input.hua",sites:l.from_slice([b.Site{node:0,module:"input.hua",origin:0,start:0,end:0,line:1,column:1,role:"source"}]),structures:l.from_slice(structures),functions:l.from_slice(functions),initializer:b.Code{name:"<initialize>",instructions:code},instructions:2,work:2}
 let verified=unwrap(v.verify(image))
 print(is_err(r.success(image,m,"input.hua",verified)))
}
""",encoding='utf-8')
for action in ('run',) if smoke else ('interpret','run'):
    assert invoke([hua,action,capacity,'--execution-profile','compiler','--',src]).stdout=='true\n'
    checks+=1
rows.append({'case':'complete report capacity','status':'rejected'})
for tail,code in [(['--budget','slice'],2),(['-o',project/'x.huab'],2)]:
    invoke([sys.executable,repo/'tools/hua_frontend.py','emit',src,*tail,'--hua',hua],code);checks+=1
invoke([sys.executable,repo/'tools/hua_frontend.py','emit',src,'--hua',hua,'-o',target])
runner.validate_bytecode(json.loads(target.read_text()),contract);checks+=2
result={'result':'passed','mode':'smoke' if smoke else 'final','cases':len(rows),'checks':checks,'seconds':round(time.monotonic()-started,3),'rows':rows,'component_paths':'VM' if smoke else 'AST/VM/source-free HUAB7/SB1','instructions':'Hua generated; every field compared against independent C05 host compiler','execution':'production AST/VM vs source-free Hua instruction VM and C++-encoded HUAB','C07_encoder':'pending; adapter decodes only, C++ writes container'}
(out/('smoke-results.json' if smoke else 'bytecode-results.json')).write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k!='rows'},ensure_ascii=True))
