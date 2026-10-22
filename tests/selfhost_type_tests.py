from process_guard import run as run_process, guarded_main
"""A11/C04: independent semantics, four execution paths, failure publication."""
from pathlib import Path
import subprocess,json,re,shutil,struct,sys,time,importlib.util,io
from contextlib import redirect_stderr
from unittest.mock import patch
hua,probe,oracle,repo,build=map(Path,sys.argv[1:])
out=build/'a11-c04';out.mkdir(exist_ok=True);work=out/'acceptance';work.mkdir(exist_ok=True)
rows=[];checks=0
def invoke(args,code=0,timeout=600):
    p=run_process(list(map(str,args)),capture_output=True,text=True,encoding='utf-8',timeout=timeout)
    assert p.returncode==code,(args,p.returncode,p.stdout[:1500],p.stderr)
    return p
def diag(d):
    x=re.match(r'(E\d+)@(\d+):(\d+)',d['error']);assert x,d
    return x[1],int(x[2]),int(x[3])
closure=work/'persisted';closure.mkdir(exist_ok=True)
for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,closure/p.name)
artifacts={}
for name in ['parser_compiler_main','modules_compiler_main','types_main']:
    invoke([probe,'check',repo/'selfhost'/f'{name}.hua']);checks+=1
    target=closure/(name+'.huab');invoke([probe,'build',closure/f'{name}.hua','-o',target])
    assert struct.unpack_from('<II',target.read_bytes(),8)==(7,1);checks+=1
    artifacts[name]=target
for p in closure.glob('*.hua'):p.unlink()
assert not list(closure.glob('*.hua'));checks+=1
def run(entry,path,parse=False):
    args=[path,'input.hua'] if parse else [path]
    reports=[]
    for exe,action,target in [(hua,'interpret',repo/'selfhost'/f'{entry}.hua'),(hua,'run',repo/'selfhost'/f'{entry}.hua'),(hua,'run',artifacts[entry]),(probe,'run',repo/'selfhost'/f'{entry}.hua')]:
        p=invoke([exe,action,target,'--execution-profile','compiler','--report-budget','--',*args])
        report=json.loads(p.stdout);metrics=json.loads(p.stderr)
        assert metrics['profile']=='compiler' and metrics['per_executor_step_limit']==100000000000 and metrics['call_depth_limit']==128 and metrics['collection_interval']==65536
        assert 0<metrics['steps']<=100000000000 and 0<metrics['peak_calls']<=128
        reports.append(report)
    assert all(x==reports[0] for x in reports),(entry,path,reports)
    return reports[0]
positive={
'array_identity':'fn f(x [2]int) int{return x[1]}\nfn main(){let a [2]int=[1,2]\nprint(f(a))}\n',
'array_parameters':'fn f(x [2]int) int{return x[1]}\nfn g(x [2]int) int{return x[1]}\n',
'map_array':'fn main(){let a map[string][2]int=map[string][2]int{"x":[1,2]}\nprint(type(a),type(unwrap(a["x"])),unwrap(a["x"])[1])}\n',
'scalars':'fn main(){var x int=2\nx+=1\nlet y=x/2\nprint(y)}\n',
'sizes':'fn f(x i16,y u8) i32{return i32(x)+i32(y)}\nfn main(){let x i8=3\nlet a [2]int=[1,2]\nprint(f(x,byte(1)),sizeof(a))}\n',
'struct':'struct A{x int}\nfn f(a A) int{return a.x}\nfn main(){var a=A{x:1}\na.x=2\nprint(f(a))}\n',
'container':'import std.list as l\nfn main(){var x []int=[]\nvar a=l.from_slice(x)\nl.append(a,1)\nlet y=l.snapshot(a)\nprint(y[0])}\n',
'map':'fn main(){var m=map[string]int{"a":1}\nm["b"]=2\nlet x=unwrap_or(m["a"],0)\nprint(x,has(m,"b"),delete(m,"b"))}\n',
'enum':'enum E{A(int),B}\nfn f(x E) int{match x{A(v)=>{return v}\nB=>{return 0}}}\nfn main(){print(f(E.A(2)))}\n',
'multi':'fn f()(int,string){return 1,"a"}\nfn main(){var a,b=f()\na,_=2,"b"\nprint(a,b)}\n',
'result':'fn g() Result<int>{return ok(2)}\nfn f() Result<void>{let x=g()?\ndefer print(x)\nreturn ok()}\nfn main(){unwrap(f())}\n',
'optional':'fn f(x int?) int{if x==nil{return 0}\nreturn x+1}\nfn main(){print(f(nil))}\n',
'and':'fn f(x int?) bool{return is_some(x)&&x>0}\n',
'capchain':'fn a() []int{return b()}\nfn b() []int{return [1]}\nfn change(x mut []int){x[0]=2}\nfn main(){var x=a()\nchange(x)}\n',
'capmut':'fn borrow(x mut []int) []int{return x}\nfn main(){var x=[1]\nvar y=borrow(x)\ny[0]=2}\n',
'clone':'fn f(x []int) []int{return clone(x)}\nfn main(){let x=[1]\nvar y=f(x)\ny[0]=2}\n',
'array_context':'fn f() [2]f32{return [1,2]}\n',
'loop':'fn f(x []int) int{var sum=0\nfor i,v in x{sum+=v}\nreturn sum}\n',
'slice':'fn main(){var x=[1,2]\nvar y=x[:]\ny[0]=3}\n',
'json':'import std.json as j\nfn f(x j.Value) Result<string>{return j.encode(x)}\n',
'buffer':'import std.buffer as b\nfn main(){var x=b.new()\nb.write_text(x,"a")\nprint(b.len(x))}\n',
'branch_stop':'fn f(x int?) int{if is_none(x){return 0}else{return x}}\n',
 'optional_match':'fn f(x int?) int{match x{nil=>{return 0}\n_=>{return x}}}\n',
 'while_guard':'fn f(x int?) int{var y int?=x\nwhile y==nil{y=1}\nreturn y}\n',
 'loop_narrow':'fn f(x int?) int{if x!=nil{for i in 0..2{print(x+1)}\nreturn x}\nreturn 0}\n',
 'bool_match':'fn f(x bool) int{match x{true=>{return 1}\nfalse=>{return 0}}}\n',
 'global_function_alias':'fn base(x int) int{return x+1}\nlet alias=base\nfn main(){print(alias(2),base(3))}\n',
 'global_enum_alias':'enum E{A(int),B}\nlet factory=E.A\nfn f() E{return factory(2)}\n',
 'for_discard':'fn main(){let data=map[string]bool{\"a\":true}\nfor key,_ in data{print(key)}}\n',
 'global_data_branches':'var data=[1]\nfn f(flag bool){if flag{data[0]=2}else{data[0]=3}\nfor i in 0..1{data[i]=4}}\n',

}
negative={
'badmain':'fn main(x int){}\n',
'capread':'fn a(x []int) []int{return x}\nfn main(){let z=[1]\nvar x=a(z)\nx[0]=2}\n',
'capbranch':'fn main(){let ro=[1]\nvar x=[2]\nif true{x=ro}\nx[0]=3}\n',
'caploop':'fn main(){let ro=[1]\nvar x=[2]\nwhile false{x=ro}\nx[0]=3}\n',
'let':'fn main(){let x=1\nx=2}\n',
'badfield':'struct A{x int}\nlet a=A{x:"bad"}\n',
'unknownfield':'struct A{x int}\nlet a=A{y:1}\n',
'badarg':'fn f(x int) int{return x}\nlet a=f("x")\n',
'arity':'fn f(x int) int{return x}\nlet a=f()\n',
'badreturn':'fn f() int{return "bad"}\n',
'missingreturn':'fn f(x bool) int{if x{return 1}}\n',
'badlist':'import std.list as l\nfn main(){var a=l.from_slice([1])\nl.append(a,"bad")}\n',
'void':'let x=print(1)\n',
'nonexhaustive':'enum E{A,B}\nfn f(x E){match x{A=>{print(1)}}}\n',
'badmut':'fn f(x mut []int){}\nfn main(){let x=[1]\nf(x)}\n',
'badmath':'import std.math as m\nlet x=m.sin(1)\n',
'double_optional':'let x Result<int>?=nil\n',
'badpropagate':'fn f() int{let r Result<int>=ok(1)\nreturn r?}\n',
'badcondition':'fn main(){if 1{print(1)}}\n',
'float_integerop':'let x=1.0//2.0\n',
'result_void_value':'fn f() Result<int>{return ok()}\n',
'stdreadonly':'import std.list as l\nfn main(){let a=l.from_slice([1])\nl.append(a,2)}\n',
'compound_division':'fn main(){var x=1\nx/=2}\n',
'multi_arity':'fn f()(int,string){return 1}\n',
 'match_numeric_duplicate':'fn f(x int){match x{1=>{print(1)}\n01=>{print(2)}\n_=>{print(0)}}}\n',
 'match_duplicate':'fn f(x bool) int{match x{true=>{return 1}\ntrue=>{return 2}\n_=>{return 0}}}\n',
}
excluded={'const':'const x=1\n','untyped':'fn f(x){return x}\n','unknown_inferred':'let x=[]\n','interface':'interface A{}\n','task':'async fn f(){}\n','error':'let x Result<int,std.error.Value>=nil\n'}
def type_case(name,text,phase,files=None):
    global checks
    project=work/name;project.mkdir(exist_ok=True);source=project/'input.hua';source.write_text(text,encoding='utf-8',newline='\n')
    for file,content in (files or {}).items():p=project/file;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content,encoding='utf-8',newline='\n')
    ref=json.loads(invoke([oracle,'--types',source]).stdout);result=run('types_main',source)
    if phase=='positive':
        assert result['status']==ref['status']=='ok',(name,result,ref)
        reference={}
        for fact in ref['facts']:reference.setdefault((fact['module'],fact['start'],fact['end']),[]).append(fact)
        comparisons=0
        for fact in result['facts']:
            options=reference.get((fact['module'],fact['start'],fact['end']),[])
            if not options:continue
            assert any(fact['type_name']==x['type_name'] and (x['writable'] is None or fact['writable']==x['writable']) for x in options),(name,fact,options)
            comparisons+=1
        assert comparisons>0;checks+=comparisons
        funcs={x['target']:x['writable'] for x in ref['functions']}
        for f in result['functions']:assert f['writable']==funcs[f['target']],(name,f,funcs);checks+=1
    else:
        assert result['status']=='error' and not any(k in result for k in ('graph','facts','symbols','functions'))
        if phase=='negative':assert diag(result)==(ref['code'],ref['line'],ref['column']),(name,result,ref)
        else:assert diag(result)[0]=='E8201',(name,result)
    checks+=7;rows.append({'case':name,'phase':phase,'diagnostic':result.get('error'),'facts':len(result.get('facts',[])),'paths':'AST/VM/source-free HUAB/SB1'})
    return source,result
negative.update({c['name']:c['source'] for c in json.loads((repo/'tests/data/a02_cases.json').read_text(encoding='utf-8')) if c['name'].startswith('array_syntax_')})
for phase,cases in [('positive',positive),('negative',negative),('excluded',excluded)]:
    for name,text in cases.items():type_case(name,text,phase)
    print('C04',phase,'passed',flush=True)
source,report=type_case('imported','import model as m\nfn main(){var x=m.f()\nx.a[0]=3}\n','positive',{'model.hua':'pub struct X{a []int}\npub fn f() X{return X{a:[1]}}\n'})
# Analyzer reads source without executing its initializers or function bodies.
side=work/'no-effects.hua';sentinel=work/'effect.txt';sentinel.unlink(missing_ok=True)
side.write_text('import std.fs as f\nprint("MUST_NOT_PRINT")\nf.write_text('+json.dumps(str(sentinel).replace('\\','/'))+',"changed")\n',encoding='utf-8')
result=run('types_main',side);assert result['status']=='ok' and not sentinel.exists();checks+=5
# Standard signature metadata is checked against independent native registration, not copied expected values.
actual=[]
for line in (repo/'selfhost/signatures.hua').read_text(encoding='utf-8').splitlines():
    m=re.search(r'if target == ("[^"]+") \{ return Signature\{parameters:(\[.*?\]),result:(".*?"),mutates:(true|false),writable:(true|false)\}',line)
    if m:actual.append({'target':json.loads(m[1]),'parameters':json.loads(m[2]),'result':json.loads(m[3]),'mutates':m[4]=='true','writable':m[5]=='true'})
assert actual==json.loads(invoke([oracle,'--signatures']).stdout) and len(actual)==108;checks+=2
# Check ordinary defaults and argument separation, persisted invocation policy, fail-before-output.
budget=work/'loop-budget.hua';budget.write_text('fn main(){var i=0\nwhile i<150000{i+=1}\nprint(i)}\n',encoding='utf-8')
for action in ('run','interpret'):
    p=invoke([hua,action,budget,'--report-budget'],1)
    metric=json.loads(p.stderr.splitlines()[-1]);assert '[E4099]' in p.stderr and metric['profile']=='default' and metric['steps']==1000001 and metric['collection_interval']==256 and not p.stdout;checks+=2
    p=invoke([hua,action,budget,'--execution-profile','compiler','--report-budget'])
    assert p.stdout.strip()=='150000' and json.loads(p.stderr)['steps']>1000000;checks+=2
for args in [['--execution-profile','unlimited'],['--execution-profile','compiler','--execution-profile','default'],['--report-budget','--report-budget']]:
    invoke([hua,'run',budget,*args],2);checks+=1
# The explicit policy follows background Task frames across cooperative resumes.
task_budget=work/'task-budget.hua'
task_budget.write_text('import std.task as t\nasync fn child() int{var i=0\nwhile i<150000{i+=1\nif i%10000==0{t.yield()}}\nreturn i}\nfn main(){let task=child()\nprint(await task)}\n',encoding='utf-8')
task_artifact=work/'task-budget.huab'
invoke([hua,'build',task_budget,'-o',task_artifact])
assert struct.unpack_from('<II',task_artifact.read_bytes(),8)==(6,1);checks+=1
for action,target in [('interpret',task_budget),('run',task_budget),('run',task_artifact)]:
    if target==task_artifact:task_budget.unlink()
    p=invoke([hua,action,target,'--report-budget'],1)
    metric=json.loads(p.stderr.splitlines()[-1])
    assert '[E4099]' in p.stderr and metric['profile']=='default' and metric['steps']>1000000 and not p.stdout;checks+=2
    p=invoke([hua,action,target,'--execution-profile','compiler','--report-budget'])
    metric=json.loads(p.stderr)
    assert p.stdout.strip()=='150000' and metric['steps']>1000000 and metric['profile']=='compiler' and metric['collection_interval']==65536;checks+=2

# Compiler budgets are explicit and old small profile remains bounded.
large=work/'large.hua';large.write_text('#'+'x'*8192+'\nlet x=1\n',encoding='utf-8')
p=invoke([sys.executable,repo/'tools/hua_frontend.py','parse',large,'--profile','C02-P2','--hua',hua],1);assert 'E8202' in p.stderr;checks+=1
p=invoke([sys.executable,repo/'tools/hua_frontend.py','parse',large,'--profile','C02-P2','--budget','compiler','--hua',hua]);assert json.loads(p.stdout)['budget_profile']=='compiler';checks+=1
huge=work/'huge.hua';huge.write_bytes(b'#'+b'x'*131072)
old=work/'old.json';old.write_text('KEEP',encoding='utf-8')
p=invoke([sys.executable,repo/'tools/hua_frontend.py','check',huge,'--hua',hua,'-o',old],1);assert 'E8202' in p.stderr and old.read_text()=='KEEP';checks+=2
# Wrapper succeeds atomically and rejects malformed or partial typed reports.
p=invoke([sys.executable,repo/'tools/hua_frontend.py','check',source,'--hua',hua,'-o',old]);assert json.loads(old.read_text())==report;checks+=2
spec=importlib.util.spec_from_file_location('frontend',repo/'tools/hua_frontend.py');frontend=importlib.util.module_from_spec(spec);spec.loader.exec_module(frontend)
for mutate in [lambda x:x['symbols'].pop(),lambda x:x['facts'][0].update(node=-1),lambda x:x['functions'][0].update(writable='yes'),lambda x:x['graph'].update(budget_profile='slice'),lambda x:x.update(status='error',stage='types',error='E3004@1:1 bad')]:
    bad=json.loads(json.dumps(report));mutate(bad);old.write_text('KEEP',encoding='utf-8')
    with patch.object(frontend.subprocess,'run',return_value=subprocess.CompletedProcess([],0,json.dumps(bad),'')),redirect_stderr(io.StringIO()):
        code=guarded_main(frontend,['check',str(source),'--hua',str(hua),'-o',str(old)])
    assert code==3 and old.read_text()=='KEEP';checks+=2
result={'cases':len(rows),'checks':checks,'rows':rows,'signature_count':len(actual),'source_free_huab':True}
(out/'types-results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(f'A11/C04: {len(rows)} cases / {checks} checks passed',flush=True)
