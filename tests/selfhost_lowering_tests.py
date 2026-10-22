from process_guard import run as run_process, guarded_main
"""C05: Hua lowered core, independent host behavior, four component paths, publication."""
from pathlib import Path
import subprocess,sys,json,struct,shutil,re,importlib.util,copy,io,time
from unittest.mock import patch
from contextlib import redirect_stderr
hua,probe,oracle,driver,repo,build=map(Path,sys.argv[1:])
out=build/'c05';out.mkdir(exist_ok=True);work=out/'acceptance';work.mkdir(exist_ok=True)
rows=[];checks=0
def invoke(args,code=0,timeout=600):
    p=run_process(list(map(str,args)),capture_output=True,text=True,encoding='utf-8',timeout=timeout)
    assert p.returncode==code,(args,p.returncode,p.stdout[:1800],p.stderr)
    return p
spec=importlib.util.spec_from_file_location('c05_runner',repo/'tools/hua_frontend.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
contract=json.loads((repo/'selfhost/contract.json').read_text(encoding='utf-8'))
contract['budgets']=contract['compiler_budget']['parser'];contract['analysis']['budgets']=contract['compiler_budget']['analysis']
closure=work/'persisted';closure.mkdir(exist_ok=True)
for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,closure/p.name)
entry=repo/'selfhost/lowering_main.hua';artifact=closure/'lowering.huab'
invoke([probe,'check',entry]);invoke([probe,'build',closure/'lowering_main.hua','-o',artifact])
assert struct.unpack_from('<II',artifact.read_bytes(),8)==(7,1);checks+=2
for p in closure.glob('*.hua'):p.unlink()
assert not list(closure.glob('*.hua'));checks+=1
def run(source):
    reports=[]
    for exe,action,target in [(hua,'interpret',entry),(hua,'run',entry),(hua,'run',artifact),(probe,'run',entry)]:
        p=invoke([exe,action,target,'--execution-profile','compiler','--report-budget','--',source])
        reports.append(json.loads(p.stdout));metrics=json.loads(p.stderr)
        assert metrics['profile']=='compiler' and 0<metrics['steps']<=100000000000 and 0<metrics['peak_calls']<=128
    assert all(x==reports[0] for x in reports)
    return reports[0]
positive={
'callable_arrays':('import std.list as l\nfn main(){let a=[str]\nlet b=[l.len]\nprint(type(a),type(b))}\n',None),
'callable_values':('import std.list as l\nenum E{A(int)}\nfn f(x int) int{return x}\nfn main(){let a=f\nlet b=E.A\nlet c=str\nlet d=l.len\nprint(type(a),type(b),type(c),type(d))}\n',None),
'arithmetic':('fn main(){print(7/2,-7//2,-7%2,2**3,1<<3)}\n','3.5 -4 1 8 8\n'),
'precise':('fn f(x i16,y u8) i32{return i32(x)+i32(y)}\nfn main(){let a [2]f32=[1,2]\nprint(f(i8(3),byte(1)),sizeof(a),alignof(a))}\n','4 8 4\n'),
'forward':('fn f(x int) int{return g(x)+1}\nfn g(x int) int{return x*2}\nfn main(){print(f(3))}\n','7\n'),
'shadow':('fn main(){var x=1\nif true{let x=2\nprint(x)}\nprint(x)}\n','2\n1\n'),
'shortcircuit':('fn touch() bool{print("BAD")\nreturn true}\nfn main(){print(false&&touch(),true||touch())}\n','false true\n'),
'order':('fn f(x int) int{print(x)\nreturn x}\nfn add(x int,y int) int{return x+y}\nfn main(){print(add(f(1),f(2)),f(3)+f(4))}\n','1\n2\n3\n4\n3 7\n'),
'location':('fn at() int{print("index")\nreturn 0}\nfn rhs() int{print("rhs")\nreturn 4}\nfn main(){var x=[1]\nx[at()]+=rhs()\nprint(x[0])}\n','index\nrhs\n5\n'),
'while':('fn main(){var i=0\nvar total=0\nwhile i<6{i+=1\nif i==2{continue}\nif i==5{break}\ntotal+=i}\nprint(total,i)}\n','8 5\n'),
'range':('fn main(){var s=0\nfor i in 1..=5 by 2{s+=i}\nfor i in 5..0 by -2{s+=i}\nprint(s)}\n','18\n'),
'for_slice':('fn main(){var s=0\nfor i,v in [2,3]{s+=i+v}\nprint(s)}\n','6\n'),
'for_map':('fn main(){let m=map[string]int{"b":2,"a":1}\nfor k,v in m{print(k,v)}}\n','a 1\nb 2\n'),
'containers':('import std.list as l\nfn main(){var x []int=[]\nvar a=l.from_slice(x)\nl.append(a,2)\nvar m=map[string]int{"a":1}\nm["b"]=3\nprint(unwrap(l.get(a,0)),unwrap(m["b"]),has(m,"a"),delete(m,"a"))}\n','2 3 true true\n'),
'slice_mut':('fn borrow(x mut []int) []int{return x}\nfn main(){var x=[1,2,3]\nvar y=borrow(x)[1:]\ny[0]=4\nprint(x[1])}\n','4\n'),
'struct':('struct A{x int}\nfn inc(a mut A){a.x+=1}\nfn main(){var a=A{x:1}\ninc(a)\nprint(a.x)}\n','2\n'),
'enum':('enum E{A(int,string),B,C(bool)}\nfn f(x E) string{match x{E.A(v,s)=>{return s+str(v)}\nB=>{return "B"}\nC(b)=>{return str(b)}}}\nfn main(){print(f(E.A(2,"a")),f(E.B()),f(E.C(true)))}\n','a2 B true\n'),
'match_scalar':('fn f(x int) string{match x{01=>{return "one"}\n_=>{return "other"}}}\nfn b(x bool) int{match x{true=>{return 1}\nfalse=>{return 0}}}\nfn main(){print(f(1),f(2),b(false))}\n','one other 0\n'),
'optional':('fn f(x int?) int{if x==nil{return 0}\nreturn x+1}\nfn g(x int?) int{match x{nil=>{return 4}\n_=>{return x}}}\nfn main(){print(f(nil),f(2),g(nil),g(3))}\n','0 3 4 3\n'),
'multi':('fn f()(int,string){return 1,"a"}\nfn main(){var a,b=f()\na,_=2,"b"\nprint(a,b)}\n','2 a\n'),
'result':('fn g() Result<int>{return ok(2)}\nfn f() Result<void>{let x=g()?\ndefer print(x)\nreturn ok()}\nfn main(){unwrap(f())}\n','2\n'),
'propagate_error':('fn fail() Result<int>{return err("bad")}\nfn f() Result<int>{defer print("cleanup")\nlet x=fail()?\nprint("BAD")\nreturn ok(x)}\nfn main(){print(unwrap_err(f()))}\n','cleanup\nbad\n'),
'defer_capture':('fn f() int{var x=1\ndefer print(x)\nx=2\ndefer print(x)\nreturn x}\nfn main(){print(f())}\n','2\n1\n2\n'),
'defer_loop':('fn main(){for i in 0..3{defer print(i)\nif i==1{continue}\nif i==2{break}\nprint("body",i)}}\n','body 0\n0\n1\n2\n'),
'json_buffer':('import std.json as j\nimport std.buffer as b\nfn main(){let x=unwrap(j.decode("{\\"a\\":1}"))\nvar buf=b.new()\nb.write_text(buf,unwrap(j.encode(x)))\nprint(unwrap(b.text(buf)))}\n','{"a":1}\n'),
'escapes':('fn main(){print("\\\\r","中文",type(1))}\r\n','\\r 中文 int\n'),
'empty':('','')
}
runtime_errors={
'index_before_rhs':('fn rhs() int{print("BAD")\nreturn 2}\nfn main(){let a=[1]\nprint(a[2]+rhs())}\n'),
'error_cleanup':('fn f(){defer print("second")\ndefer print("first")\nprint(1//0)}\nfn main(){f()}\n'),
'cleanup_priority':('fn broken(){print("cleanup")\nprint(2//0)}\nfn f(){defer print("last")\ndefer broken()\nprint(1//0)}\nfn main(){f()}\n'),
'overflow':('fn main(){var x i8=127\nx+=1\nprint(x)}\n')
}
negative={
'readonly':('fn main(){let x=[1]\nx[0]=2}\n','E3003'),
'const':('print("BAD")\nconst x=1\n','E8201'),
'nested':('fn main(){fn f(){print(1)}\nf()}\n','E8201'),
'nonexhaustive':('enum E{A,B}\nfn f(x E){match x{A=>{print(1)}}}\n','E3011'),
'badreturn':('fn f() int{return "bad"}\n','E3004'),
'badmain':('fn main(x int){}\n','E3005'),
'unsafe':('unsafe {print(1)}\n','E8201'),
'async':('async fn f(){}\n','E8201')
}
def case(name,text,expected=None,error=None,files=None,source_name="input.hua"):
    global checks
    project=work/name;project.mkdir(exist_ok=True);source=project/source_name;source.write_text(text,encoding='utf-8',newline='')
    for name_,body in (files or {}).items():
        p=project/name_;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body,encoding='utf-8',newline='\n')
    report=run(source)
    if error:
        assert report['status']=='error' and report['error'].startswith(error+'@'),(name,report)
        assert not any(k in report for k in ('nodes','analysis','initialization','sources'))
        checks+=6;rows.append({'case':name,'status':'rejected','error':report['error']});return report
    assert report['status']=='ok',(name,report);runner.validate_lowering(report,contract)
    assert not any(n['kind'] in ('Program','Import','Parameters','Enum','Variant','Const') for n in report['nodes'])
    # An independent production frontend checks acceptance and behavior before input is removed.
    invoke([hua,'check',source])
    reference=[invoke([hua,cmd,source],1 if name in runtime_errors else 0) for cmd in ('interpret','run')]
    assert reference[0].stdout==reference[1].stdout
    if expected is not None:assert reference[0].stdout==expected,(name,reference[0].stdout,expected)
    dest=project/'core.json';dest.write_text(json.dumps(report,ensure_ascii=False),encoding='utf-8')
    artifact_=project/'core.huab';invoke([driver,'build',dest,'-o',artifact_])
    header=struct.unpack_from('<II',artifact_.read_bytes(),8);assert header[0] in (6,7) and header[1]==1
    if name=='result':assert header[0]==7
    # The core/adapter paths now have no original .hua files to consult.
    for p in project.rglob('*.hua'):p.unlink()
    for exe,cmd,target in [(driver,'interpret',dest),(driver,'run',dest),(hua,'run',artifact_)]:
        result=invoke([exe,cmd,target],1 if name in runtime_errors else 0)
        assert result.stdout==reference[0].stdout,(name,result.stdout,reference[0].stdout)
        if name in runtime_errors:
            code=re.search(r'error\[(E\d+)\]',reference[0].stderr)[1]
            assert 'error['+code+']' in result.stderr,(name,code,result.stderr)
            pos=re.search(r':(\d+):(\d+)',reference[0].stderr)
            assert f'input.hua:{pos[1]}:{pos[2]}' in result.stderr,(name,reference[0].stderr,result.stderr)
    checks+=13+len(report['nodes']);rows.append({'case':name,'status':'ok','nodes':len(report['nodes']),'huab':header[0],'output':reference[0].stdout})
    return report
started=time.monotonic()
saved={}
for name,(text,expected) in positive.items():saved[name]=case(name,text,expected)
for name,text in runtime_errors.items():saved[name]=case(name,text)
for name,(text,code) in negative.items():case(name,text,error=code)
case('modules','import model as m\nfn main(){print(m.get(m.E.A(3)))}\n','common\nmodel\n3\n',files={
'common.hua':'print("common")\npub struct Data{x int}\n',
'model.hua':'import common as c\nprint("model")\npub enum E{A(int),B}\npub fn get(x E) int{match x{A(v)=>{return v}\nB=>{return 0}}}\n'})
case('module_suffix','import model as a\nimport pkg.model as b\nfn main(){let x=a.A{x:1}\nlet y=b.A{x:2}\nprint(a.get(x),b.get(y))}\n','1 2\n',files={
'model.hua':'pub struct A{x int}\npub fn get(a A) int{return a.x}\n',
'pkg/model.hua':'pub struct A{x int}\npub fn get(a A) int{return a.x}\n'})
case('special_module_name','struct A{x int}\nfn main(){print(type(A{x:1}))}\n','A\n',source_name='带 #空格$.hua')
# Mutate the real Hua Core through its supported list interface; exercise its verifier independently.
verification=work/'verifier';verification.mkdir(exist_ok=True)
for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,verification/p.name)
verification.joinpath('verify_main.hua').write_text("""
import lowering as l
import types as t
import lowering_report as report
import std.os as os
import std.list as list
fn node(k string, children []int) l.Node {
    return l.Node{id:0,module:"input.hua",origin:0,start:0,end:0,line:1,column:1,kind:k,text:"",type_name:"",writable:false,role:"source",children:children}
}
fn test(k string, children []int) {
    var c=l.Core{nodes:list.from_slice([node(k,children)]),initialization:list.from_slice([l.Initializer{module:"input.hua",roots:[0]}]),entry:"input.hua",work:1}
    print(unwrap_err(l.verify(c)))
}
fn main(){
    test("While",[])
    test("Block",[0])
    test("Break",[])
    test("Return",[])
    test("Call",[])
    test("Let",[])
    var c=l.Core{nodes:list.from_slice([node("Block",[])]),initialization:list.from_slice([l.Initializer{module:"input.hua",roots:[]}]),entry:"input.hua",work:1}
    print(unwrap_err(l.verify(c)))
    var wrong=node("Unary",[0])
    wrong.id=1
    list.append(c.nodes,wrong)
    list.set(c.initialization,0,l.Initializer{module:"input.hua",roots:[1]})
    print(unwrap_err(l.verify(c)))
    c.work=1048577
    print(unwrap_err(l.verify(c)))
    var huge="x"
    for i in 0..24 { huge+=huge }
    var value=node("String",[])
    value.text=huge
    var big=l.Core{nodes:list.from_slice([value]),initialization:list.from_slice([l.Initializer{module:"input.hua",roots:[0]}]),entry:"input.hua",work:1}
    let model=unwrap(t.analyze(os.args()[0]))
    print("REPORT_BUDGET",is_err(report.success(big,model,"input.hua")))
}
""",encoding='utf-8',newline='\n')
invoke([probe,'check',verification/'verify_main.hua'])
vp=verification/'verify.huab';invoke([probe,'build',verification/'verify_main.hua','-o',vp])
budget_input=work/'report_input.hua';budget_input.write_text('',encoding='utf-8')
before=[]
for cmd in ('interpret','run'):before.append(invoke([hua,cmd,verification/'verify_main.hua','--',budget_input]).stdout)
for p in verification.glob('*.hua'):p.unlink()
after=invoke([hua,'run',vp,'--',budget_input]).stdout
assert before[0]==before[1]==after and len(after.splitlines())==10
assert all(x.startswith('E8203@') for x in after.splitlines()[:-2]) and after.splitlines()[-2].startswith('E8202@') and after.splitlines()[-1]=='REPORT_BUDGET true'
checks+=14
# Equivalent logical tree after moving the source directory (including normalized CRLF).
a=work/'determinism1';b=work/'determinism2';a.mkdir(exist_ok=True);b.mkdir(exist_ok=True)
for p in (a/'input.hua',b/'input.hua'):p.write_bytes(b'\xef\xbb\xbffn main(){print("ok")}\r\n')
r1=run(a/'input.hua');r2=run(b/'input.hua');assert r1==r2;checks+=5
# Protocol mutations must fail before replacing a previous artifact.
valid=saved['enum'];mutations=[]
def bad(label,change):
    x=copy.deepcopy(valid);change(x);mutations.append((label,x))
bad('id',lambda x:x['nodes'][0].update(id=True))
bad('forward',lambda x:x['nodes'][-1]['children'].append(len(x['nodes'])-1))
bad('wrong_type',lambda x:next(n for n in x['nodes'] if n['role']=='source' and n['kind']=='Call').update(type_name='BAD'))
bad('symbol_mapping',lambda x:next(n for n in x['nodes'] if n['role']=='source' and n['kind']=='Function').update(text='BAD'))
bad('missing_type',lambda x:next(n for n in x['nodes'] if n['kind']=='Call').update(type_name=''))
bad('orphan',lambda x:x['initialization'][0]['roots'].pop())
bad('shape',lambda x:x['nodes'][0].update(kind='While'))
bad('span',lambda x:x['nodes'][0].update(column=100000))
bad('role',lambda x:x['nodes'][0].update(role='unknown'))
bad('source',lambda x:x['sources'][0].update(text=''))
bad('module',lambda x:x['nodes'][0].update(module='unknown.hua'))
bad('budget',lambda x:x['stats'].update(work=1048577))
bad('signature',lambda x:next(n for n in x['nodes'] if n['kind']=='Function')['children'].reverse())
old=a/'output.json'
for label,report in mutations:
    old.write_text('KEEP',encoding='utf-8')
    fake=subprocess.CompletedProcess([],0,json.dumps(report),'')
    with patch.object(runner.subprocess,'run',return_value=fake),redirect_stderr(io.StringIO()) as err:
        rc=guarded_main(runner,['lower',str(a/'input.hua'),'-o',str(old),'--hua',str(hua)])
    assert rc==3 and old.read_text()=='KEEP',(label,rc,err.getvalue())
    checks+=2
# Source rejection / wrong CLI options also preserve the previous output.
a.joinpath('bad.hua').write_text('const x=1\n',encoding='utf-8')
for arguments,code in [(['lower',a/'bad.hua'],1),(['lower',a/'input.hua','--budget','slice'],2),(['lower',a/'input.hua','-o',a/'x.huab'],2)]:
    if '-o' not in list(map(str,arguments)):arguments += ['-o',old]
    old.write_text('KEEP',encoding='utf-8')
    invoke([sys.executable,repo/'tools/hua_frontend.py',*arguments,'--hua',hua],code);assert old.read_text()=='KEEP';checks+=2
# Real publication succeeds; source initialization was never executed during lowering.
invoke([sys.executable,repo/'tools/hua_frontend.py','lower',a/'input.hua','-o',old,'--hua',hua])
runner.validate_lowering(json.loads(old.read_text()),contract);checks+=2
result={'cases':len(rows),'checks':checks,'seconds':round(time.monotonic()-started,3),'rows':rows,
        'component_paths':'AST/VM/source-free HUAB7/SB1','core_paths':'source-free core AST/core VM/adapter HUAB6-or-7',
        'adapter':'test-only C++ instructions/archive; no parser/module/lowering/sema input'}
(out/'lowering-results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k!='rows'},ensure_ascii=True))
