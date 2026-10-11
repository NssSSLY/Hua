"""C02-P2/C03 acceptance: Hua implementation vs independent C++ front end."""
from pathlib import Path
import json,re,shutil,subprocess,sys,struct,importlib.util,io,os
from unittest.mock import patch
from contextlib import redirect_stderr,redirect_stdout
hua,probe,oracle,repo,build=map(Path,sys.argv[1:])
out=build/'c02-c03';out.mkdir(exist_ok=True);work=out/'cases';work.mkdir(exist_ok=True)
checks=0;rows=[];corpus=[]
def invoke(args,code=0):
    r=subprocess.run(list(map(str,args)),capture_output=True,encoding='utf-8',timeout=45)
    assert r.returncode==code,(args,r.returncode,r.stdout,r.stderr)
    return r
# Build a private closure and remove all its source files before persisted execution.
closure=work/'persisted';closure.mkdir(exist_ok=True)
for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,closure/p.name)
artifacts={}
for profile,entry in [('C02-P2','parser_extended_main.hua'),('C03-N1','modules_main.hua')]:
    invoke([probe,'check',repo/'selfhost'/entry]);checks+=1
    target=closure/(profile+'.huab');invoke([probe,'build',closure/entry,'-o',target])
    assert struct.unpack_from('<II',target.read_bytes(),8)==(7,1);checks+=1
    artifacts[profile]=target
for p in closure.glob('*.hua'):p.unlink()
assert not list(closure.glob('*.hua'));checks+=1

def diagnose(report):
    m=re.match(r'(E\d+)@(\d+):(\d+)',report['error']);assert m,report
    return m[1],int(m[2]),int(m[3])
def project(report):
    return [{k:n[k] for k in ('kind','text','start','end','line','column','depth')}|{'arity':len(n['children'])} for n in report['nodes']]
def execute(profile,path):
    entry=repo/'selfhost'/('parser_extended_main.hua' if profile=='C02-P2' else 'modules_main.hua')
    args=[path,'input.hua'] if profile=='C02-P2' else [path]
    results=[]
    for engine,exe,target in [('interpret',hua,entry),('run',hua,entry),('run',hua,artifacts[profile]),('run',probe,entry)]:
        results.append(json.loads(invoke([exe,engine,target,'--',*args]).stdout))
    assert all(x==results[0] for x in results),(profile,path,results)
    return results[0]
positive=[
    '"汉字😀\\n\\t\\0\\\"\\\\"\n','let x string="a"\nlet y string?=nil\n',
    'let x []int=[1,2,3]\nlet y [3]int=[1,2,3]\n',
    'let x map[string]int=map[string]int{"a":1,"b":2}\nlet y=map[string]int{"one":1}\n',
    'let x=[]\nlet y=map[string]int{}\n', 'a[0]=1\na.b+=2\na++\na--\n',
    'a[:]\na[1:]\na[:2]\na[1:2]\n','1|2^3&4<<1\n~1>>1\n',
    'pub struct Point{x int\ny string?}\nlet p=Point{x:1,y:nil}\n',
    'import pkg.data as d\nimport std.fs\nlet p=d.Point{x:1}\n',
    'fn f(mut x []int,y mut List<int>) Result<void,string>{defer print("done")\nprint(x[0])?\nreturn ok()}\n',
    'fn f(x){return x}\n','fn f()(int,string){return 1,"yes"}\n',
    'let a,b int=1,2\na,_=b,a\nvar c,d=[1],[2]\n',
    'fn f(x List<Result<int,string> >?) []int {return [1]}\n',
    'for i in 0..10 by 2{print(i)}\nfor i,x in [1,2]{print(i,x)}\n',
    'pub enum Choice{None\nSome(int)}\nfn f(c Choice) int {match c{None=>{return 0}\nSome(x)=>{return x}}}\n',
    'enum Pair{Two(int,string)}\nmatch p{Pair.Two(a,b)=>{print(a,b)}\n_=>print(0)}\n',
    'match x{1=>print(1)\n"hi"=>print(2)\ntrue=>print(3)\n_=>print(4)}\n',
    'if p.done {print(p.value)} else {while p.run {p.run=false}}\n',
    'fn f(){for x in values{if x{continue}\nbreak}\nreturn}\n',
    'let r Result<int>=ok(1)\nlet l List<string>=[]\n',
    'let x mut []int=[1]\n', 'let xs List<List<int>>=[]\n','print(Point{x:1}.x,[1][0],map[string]int{"a":1}["a"])\n',
    '\ufeff#* 中😀 #* 注释 *# *#\r\nlet s="😀"\r',
    'for i in (0..3){print(i)}\n', # range inside ordinary expression is intentionally not available; see exclusions below
]
positive.pop()
negative=['let x=[1,]\n','let x=map[string]int{"a" 1}\n','struct A{x}\n','import a as\n','for x 0..2{}\n','enum A{B(,)}\n','match x{1 print(1)}\n','let a,=1\n','fn f()(int,){}\n','x[]=1\n','"unterminated','let x [2]int=\n']
excluded=['interface X{}\n','async fn f(){}\n','@test fn f(){}\n','unsafe{}\n','fn f<T>(x T) T{return x}\n','f<int>(1)\n','fn X.f(){}\n','fn f(){fn g(){}}\n','fn f(){struct X{}}\n','let x Task<int>=nil\n','let x ptr<int>=nil\n','let x Result<int,std.error.Value>=nil\n','let x Custom<int>=nil\n']
def parser_case(name,text,phase='positive',save=True):
    global checks
    path=work/(name+'.hua');path.write_text(text,encoding='utf-8',newline='')
    ref=json.loads(invoke([oracle,path]).stdout);result=execute('C02-P2',path)
    if phase=='positive':
        assert ref['status']==result['status']=='ok',(name,ref,result)
        assert project(result)==ref['nodes'],(name,project(result),ref['nodes'])
    else:
        assert result['status']=='error' and 'nodes' not in result,(name,result)
        code,line,col=diagnose(result)
        if phase=='negative':assert (code,line,col)==(ref['code'],ref['line'],ref['column']),(name,result,ref)
        else:assert code=='E8201',(name,result)
    checks+=5
    if save:rows.append({'case':name,'profile':'C02-P2','phase':phase,'result':result,'oracle':'AST and byte spans' if phase=='positive' else 'diagnostic position' if phase=='negative' else 'explicit subset rejection','paths':'AST/VM/source-free HUAB/SB1'})
    return result
for phase,texts in [('positive',positive),('negative',negative),('excluded',excluded)]:
    for i,text in enumerate(texts):parser_case(phase+str(i),text,phase)
# Exercise real source fragments, rather than claiming this small budget accepts entire components.
for source in sorted((repo/'selfhost').glob('*.hua')):
    text=source.read_text(encoding='utf-8');raw=text.encode('utf-8')
    tree=json.loads(invoke([oracle,source]).stdout);assert tree['status']=='ok',source
    chosen=[]
    for n in tree['nodes']:
        if n['depth']==1 and n['kind'] in ('Import','Struct','Enum','Function') and 0<n['end']-n['start']<=1100:
            if n['kind'] not in [x['kind'] for x in chosen]:chosen.append(n)
    for i,n in enumerate(chosen):
        fragment=raw[n['start']:n['end']].decode('utf-8')+'\n'
        result=parser_case('corpus-'+source.stem+'-'+str(i),fragment,save=False)
        corpus.append({'source':source.name,'declaration':n['text'],'kind':n['kind'],'bytes':len(fragment.encode()),'result':'accepted'})
    # Full input acceptance / explicit ceiling is measured with VM, then independently checked for accepted files.
    result=json.loads(invoke([hua,'run',repo/'selfhost/parser_extended_main.hua','--',source,'input.hua']).stdout)
    if result['status']=='ok':assert project(result)==tree['nodes'];checks+=1
    else:assert diagnose(result)[0]=='E8202',(source,result);checks+=1
    corpus.append({'source':source.name,'whole_file':True,'bytes':len(raw),'result':result['status'],'diagnostic':result.get('error','')})
print('C02-P2 parser/corpus passed',flush=True)
# Catalog must remain exactly aligned with actual host registrations in the SB1 surface.
ref=json.loads(invoke([oracle,'--catalog']).stdout)
catalog=(repo/'selfhost/catalog.hua').read_text(encoding='utf-8')
core=json.loads(re.search(r'fn core.*?alg.contains\((\[.*?\]),name',catalog).group(1))
exports={name:json.loads(values) for name,values in re.findall(r'if module == "([^"]+)" \{ return (\[.*?\]) \}',catalog)}
assert sorted(core)==ref['core'] and exports==ref['modules'] and len(core)==38;checks+=2

projects=[
 ('basic',{'main.hua':'import data as d\nfn main(){let p=d.Point{x:1}\nprint(d.twice(p.x))}\n','data.hua':'pub struct Point{x int}\npub fn twice(x int) int{return x*2}\n'},['data.hua','main.hua']),
 ('package',{'main.hua':'import pkg\nfn main(){print(pkg.value())}\n','pkg/package.hua':'pub fn value() int{return 7}\n'},['pkg/package.hua','main.hua']),
 ('priority',{'main.hua':'import pkg\nfn main(){print(pkg.value())}\n','pkg.hua':'pub fn value() int{return 1}\n','pkg/package.hua':'pub fn value() int{return 2}\n'},['pkg.hua','main.hua']),
 ('diamond',{'main.hua':'import left as l\nimport right as r\nfn main(){print(l.value()+r.value())}\n','left.hua':'import shared as s\npub fn value() int{return s.value()}\n','right.hua':'import shared as s\npub fn value() int{return s.value()}\n','shared.hua':'pub fn value() int{return 2}\n'},['shared.hua','left.hua','right.hua','main.hua']),
 ('dotted',{'main.hua':'import pkg.data\nfn main(){print(pkg.data.value())}\n','pkg/data.hua':'pub fn value() string{return "yes"}\n'},['pkg/data.hua','main.hua']),
 ('forward',{'main.hua':'fn main(){print(later())}\nfn later() int{return n}\nlet n=2\n'},['main.hua']),
 ('scopes',{'main.hua':'let x=1\nfn main(){let x=2\n{let x=3\nprint(x)}\nprint(x)\nfor i in 0..2{print(i)}\nfor i,x in [1]{print(i,x)}}\n'},['main.hua']),
 ('initializer_shadow',{'main.hua':'let x=1\nfn main(){let x=x+1\nprint(x)}\n'},['main.hua']),
 ('mut',{'main.hua':'import std.list as l\nfn f(x mut []int,y mut List<int>){x[0]=1\nl.append(y,2)}\nfn main(){var x=[0]\nvar y=l.from_slice(x)\nf(x,y)}\n'},['main.hua']),
 ('patterns',{'main.hua':'enum Choice{None\nSome(int)}\nfn f(c Choice) int{match c{None=>{return 0}\nSome(x)=>{return x}}}\nfn main(){print(f(Choice.Some(2)))}\n'},['main.hua']),
 ('multi',{'main.hua':'fn pair()(int,string){return 1,"a"}\nfn main(){var a,b=pair()\na,_=pair()\nprint(a,b)}\n'},['main.hua']),
 ('importenum',{'main.hua':'import data as d\nfn main(){let e d.Choice=d.Choice.Some(2)\nmatch e{d.Choice.Some(x)=>print(x)\nd.Choice.None=>print(0)}}\n','data.hua':'pub enum Choice{None\nSome(int)}\n'},['data.hua','main.hua']),
 ('stdtype',{'main.hua':'import std.json as j\nfn f(x j.Value){print(j.kind(x))}\nfn main(){f(unwrap(j.decode("1")))}\n'},['main.hua']),
 ('privatenames',{'main.hua':'import a\nimport b\nfn main(){print(a.get(),b.get())}\n','a.hua':'let x=1\npub fn get() int{return x}\n','b.hua':'let x=2\npub fn get() int{return x}\n'},['a.hua','b.hua','main.hua']),
 ('shadowimport',{'main.hua':'import data as d\nfn main(){let d=Point{x:1}\nprint(d.x)}\nstruct Point{x int}\n','data.hua':'pub fn value() int{return 1}\n'},['data.hua','main.hua']),
 ('binarynames',{'main.hua':'import std.bytes as b\nimport std.buffer as buf\nimport std.math as math\nfn main(){let x=unwrap(b.u32_le(4))\nprint(b.crc32(x),math.log2(8.0))\nvar out=buf.new()\nbuf.write_u32_le(out,4)}\n'},['main.hua']),
 ('typepending',{'main.hua':'struct Point{x int}\nfn main(){let p=Point{x:"wrong"}\nprint(p.missing(1))}\n'},['main.hua']),
]
failures=[
 ('missing',{'main.hua':'import missing\n'},'E5001'),
 ('cycle',{'main.hua':'import a\n','a.hua':'import main\n'},'E5002'),
 ('late',{'main.hua':'let x=1\nimport a\n','a.hua':''},'E5004'),
 ('duplicateimport',{'main.hua':'import a\nimport a\n','a.hua':''},'E5004'),
 ('namespaceconflict',{'main.hua':'import a\nlet a=1\n','a.hua':''},'E5004'),
 ('private',{'main.hua':'import a\nfn main(){a.hidden()}\n','a.hua':'fn hidden(){}\n'},'E5003'),
 ('noexport',{'main.hua':'import a\nfn main(){a.missing()}\n','a.hua':''},'E5003'),
 ('namespacevalue',{'main.hua':'import a\nfn main(){print(a)}\n','a.hua':''},'E5003'),
 ('unknown',{'main.hua':'fn main(){print(missing)}\n'},'E3001'),
 ('localbefore',{'main.hua':'fn main(){print(x)\nlet x=1}\n'},'E3001'),
 ('scopeleak',{'main.hua':'fn main(){{let x=1}\nprint(x)}\n'},'E3001'),
 ('duplicate',{'main.hua':'let x=1\nlet x=2\n'},'E3002'),
 ('duplocal',{'main.hua':'fn main(){let x=1\nlet x=2}\n'},'E3002'),
 ('globalbefore',{'main.hua':'let a=b\nlet b=1\n'},'E3001'),
 ('exporttype',{'main.hua':'import a\nfn f(x a.value){}\n','a.hua':'pub fn value() int{return 1}\n'},'E5003'),
 ('typevalue',{'main.hua':'import a\nfn main(){print(a.Point)}\n','a.hua':'pub struct Point{x int}\n'},'E5003'),
 ('unknownstd',{'main.hua':'import std.fs as fs\nfn main(){fs.missing()}\n'},'E5003'),
 ('unknowntype',{'main.hua':'fn f(x Unknown){}\n'},'E3004'),
 ('nestedimport',{'main.hua':'fn f(){import a}\n','a.hua':''},'E8201'),
 ('dependencysyntax',{'main.hua':'import data\n','data.hua':'let x=\n'},'E2001'),
 ('external',{'main.hua':'import ext\n','ext.huam':'not executed'},'E8201'),
 ('stdoutside',{'main.hua':'import std.error\n'},'E8201'),
 ('overlap',{'main.hua':'import a\nimport a.b\n','a.hua':'','a/b.hua':''},'E8201'),
 ('dependencybytes',{'main.hua':'import data\n','data.hua':' '*8193},'E5005'),
 ('qualifieddepth',{'main.hua':'let x=1\nfn main(){print(x'+'.field'*24+')}\n'},'E5005'),
 ('patternleak',{'main.hua':'enum E{A(int)}\nfn f(e E){match e{A(x)=>print(x)}\nprint(x)}\n'},'E3001'),
 ('depth',{'main.hua':'import a\n','a.hua':'import b\n','b.hua':'import c\n','c.hua':'import d\n','d.hua':''},'E5005'),
 ('modulecount',{'main.hua':''.join('import m'+str(i)+'\n' for i in range(8)),**{'m'+str(i)+'.hua':'' for i in range(8)}},'E5005'),
]
def write_project(name,files):
    directory=work/('module-'+name);directory.mkdir(exist_ok=True)
    for rel,text in files.items():
        p=directory/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text,encoding='utf-8',newline='')
    return directory/'main.hua'
for name,files,order in projects:
    path=write_project(name,files);result=execute('C03-N1',path)
    assert result['status']=='ok' and result['initialization']==order,(name,result);checks+=5
    reference=json.loads(invoke([oracle,'--load' if name=='typepending' else '--modules',path]).stdout)
    assert reference['status']=='ok',(name,reference)
    expected={u['id']:u['declarations'] for u in reference['modules']}
    assert [x['id'] for x in result['modules']]==list(expected),(name,result,reference)
    actual={u:[{k:s[k] for k in ('name','kind','published')} for s in result['symbols'] if s['module']==u and s['scope']==0] for u in expected}
    assert actual==expected,(name,actual,expected);checks+=2
    if name=='scopes':
        assert [x['target'] for x in result['references'] if x['name']=='x']==['main.hua#s3:x','main.hua#s2:x','main.hua#s6:x'];checks+=1
    if name=='initializer_shadow':
        assert [x['target'] for x in result['references'] if x['name']=='x']==['main.hua#x','main.hua#s2:x'];checks+=1
    if name=='patterns':
        assert [x['target'] for x in result['references'] if x['name']=='x']==['main.hua#s5:x'];checks+=1
    rows.append({'case':name,'profile':'C03-N1','result':result,'oracle':'loader / semantic validity and raw declarations' if name!='typepending' else 'loader only; C04 pending','paths':'AST/VM/source-free HUAB/SB1'})
for name,files,code in failures:
    path=write_project(name,files);result=execute('C03-N1',path)
    assert result['status']=='error' and diagnose(result)[0]==code and not any(k in result for k in ('modules','symbols','references','initialization')),(name,result);checks+=5
    reference=None
    if code not in ('E8201','E5005'):
        reference=json.loads(invoke([oracle,'--modules',path]).stdout)
        assert reference['status']=='error' and reference['code']==code,(name,result,reference)
        # Namespace/declaration conflicts use the program span in this first Hua slice; oracle uses the offending declaration.
        if name!='namespaceconflict':assert diagnose(result)[1:]==(reference['line'],reference['column']),(name,result,reference)
        origin=re.search(r'\[([^\]]+\.hua)\]',result['error'])
        assert origin and origin[1]==reference['source'],(name,result,reference);checks+=2
    rows.append({'case':name,'profile':'C03-N1','result':result,'oracle':reference or 'explicit analysis bound/exclusion','paths':'AST/VM/source-free HUAB/SB1'})
print('C03 projects/diagnostics passed',flush=True)
# Same project under another physical root must emit identical names/IDs/plans.
path=work/'module-diamond/main.hua';original=execute('C03-N1',path)
relocated=work/'relocated-project';shutil.copytree(path.parent,relocated,dirs_exist_ok=True)
assert execute('C03-N1',relocated/'main.hua')==original;checks+=5
# Root-relative imports are not relative to the importing subdirectory; repeated imports initialize once.
rootcase=write_project('rootbased',{'main.hua':'import pkg.inner as i\nfn main(){print(i.value())}\n','pkg/inner.hua':'import shared\npub fn value() int{return shared.value()}\n','shared.hua':'pub fn value() int{return 3}\n'})
result=execute('C03-N1',rootcase);assert result['status']=='ok' and result['initialization']==['shared.hua','pkg/inner.hua','main.hua'];checks+=5
# Analysis must never execute input initialization, functions or main.
marker=work/'must-not-exist.txt';marker.unlink(missing_ok=True)
path=write_project('noexecute',{'main.hua':'import std.fs as fs\nfs.write_text('+json.dumps(marker.as_posix())+',"BAD")\nfn main(){print("BAD")}\n'})
result=execute('C03-N1',path);assert result['status']=='ok' and not marker.exists();checks+=5
# Case variants are one module identity on Windows; preserve first-discovered spelling.
if os.name=='nt':
    path=write_project('casecache',{'main.hua':'import data as a\nimport DATA as b\nfn main(){print(a.value(),b.value())}\n','data.hua':'pub fn value() int{return 1}\n'})
    result=execute('C03-N1',path);assert result['status']=='ok' and len(result['modules'])==2 and len(result['initialization'])==2;checks+=5
# OS-dependent symlink creation is explicit in the evidence, not silently counted as coverage.
symlink='unavailable'
path=write_project('symlink',{'main.hua':'import linked\n','data.hua':'pub fn value() int{return 1}\n'})
try:
    linked=path.parent/'linked.hua'
    if linked.is_symlink():linked.unlink()
    linked.symlink_to((path.parent/'data.hua').resolve())
except OSError as error:
    if getattr(error,'winerror',None)!=1314 and error.errno not in (1,13,95):raise
    symlink=str(error)
else:
    result=execute('C03-N1',path);assert result['status']=='error' and diagnose(result)[0]=='E8302';checks+=5;symlink='passed'

# Development runner validates reports before atomic publication and maps source/I/O/host failures.
runner=repo/'tools/hua_frontend.py';target=work/'published.json'
def command(command,path,code=0):
    args=[sys.executable,runner,command,path,'--hua',hua,'-o',target]
    if command=='parse':args.extend(['--profile','C02-P2'])
    return invoke(args,code)
path=work/'positive0.hua';command('parse',path);parser_bytes=target.read_bytes();command('parse',path);assert target.read_bytes()==parser_bytes;checks+=1
command('parse',work/'negative0.hua',1);assert target.read_bytes()==parser_bytes;checks+=1
lexical=work/'command-lexical.hua';lexical.write_text('"unterminated',encoding='utf-8')
r=command('parse',lexical,1);assert json.loads(r.stderr)['stage']=='lexer' and target.read_bytes()==parser_bytes;checks+=1
path=work/'module-diamond/main.hua';command('analyze',path);first=target.read_bytes();command('analyze',path);assert target.read_bytes()==first;checks+=1
command('analyze',work/'relocated-project/main.hua');assert target.read_bytes()==first;checks+=1
for name in ('unknown','private','cycle','depth'):
    r=command('analyze',work/('module-'+name)/'main.hua',1);assert target.read_bytes()==first and r.stdout=='';checks+=1
bad=write_project('encoding',{'main.hua':'import data\n','data.hua':''});(bad.parent/'data.hua').write_bytes(b'\xff')
command('analyze',bad,2);assert target.read_bytes()==first;checks+=1
spec=importlib.util.spec_from_file_location('frontend_extended',runner);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
valid=json.loads(first);mutations=[]
for field,value in [('initialization',['main.hua']),('symbols',[{'module':'unknown.hua','node':0}]),('modules',[]),('references',[{'module':'main.hua','node':99999}])]:
    changed=json.loads(first);changed[field]=value;mutations.append(changed)
for reply in ['not JSON',json.dumps({'status':'ok'}),*(json.dumps(x) for x in mutations)]:
    fake=subprocess.CompletedProcess([],0,reply,'')
    with patch.object(module.subprocess,'run',return_value=fake),redirect_stderr(io.StringIO()),redirect_stdout(io.StringIO()):
        code=module.main(['analyze',str(path),'--hua',str(hua),'-o',str(target)])
    assert code==3 and target.read_bytes()==first;checks+=1
with patch.object(module.os,'replace',side_effect=OSError('replace failure')),redirect_stderr(io.StringIO()),redirect_stdout(io.StringIO()):
    code=module.main(['analyze',str(path),'--hua',str(hua),'-o',str(target)])
assert code==2 and target.read_bytes()==first and not list(work.glob('published.json.tmp-*'));checks+=1
# Runtime initialization order is independently observable in an ordinary valid diamond fixture.
path=write_project('runtimeorder',{'main.hua':'import left\nimport right\nprint("main")\nfn main(){}\n','left.hua':'import shared\nprint("left")\n','right.hua':'import shared\nprint("right")\n','shared.hua':'print("shared")\n'})
result=execute('C03-N1',path);assert result['initialization']==['shared.hua','left.hua','right.hua','main.hua'];checks+=5
for engine in ('interpret','run'):
    assert invoke([hua,engine,path]).stdout=='shared\nleft\nright\nmain\n';checks+=1
report={'result':'passed','profiles':['C02-P2','C03-N1'],'platform':sys.platform,'checks':checks,'cases':len(rows),'corpus':corpus,'rows':rows,'catalog_core':len(core),'catalog_std_modules':len(exports),'symlink':symlink,'HUAB_format':7,'no_input_execution':True,'type_analysis':False,'Hua_encoder':False,'publication_exit_determinism':'passed'}
(out/'extended-results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(f'C02-P2/C03-N1: {len(rows)} cases, {len(corpus)} real-source observations, {checks} checks passed')
