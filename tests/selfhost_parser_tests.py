"""C01/C02: raw syntax only, independent AST/diagnostics, budgets and publication."""
import importlib.util,io,json,re,shutil,struct,subprocess,sys,tempfile,zlib
from contextlib import redirect_stderr,redirect_stdout
from pathlib import Path
from unittest.mock import patch
hua,probe,oracle,repo,build=[Path(x).resolve() for x in sys.argv[1:]]
out=build/'c01-c02';out.mkdir(exist_ok=True);work=out/'cases';work.mkdir(exist_ok=True)
checks=0;rows=[]
def invoke(args,code=0):
 global checks
 p=subprocess.run(list(map(str,args)),capture_output=True,encoding='utf-8',timeout=45)
 assert p.returncode==code,(args,p.returncode,p.stdout,p.stderr)
 checks+=1;return p
contract=json.loads((repo/'selfhost/contract.json').read_text(encoding='utf-8'))
entry=repo/'selfhost/parser_main.hua'
assert invoke([probe,'check',entry]).stdout=='SB1 OK\n'
# Copy every local module, build with the SB1 probe, remove the entire closure.
modules=['lexer.hua','ast_pool.hua','parser.hua','parser_report.hua','parser_main.hua']
copy=work/'seed';copy.mkdir(exist_ok=True)
for n in modules:shutil.copy2(repo/'selfhost'/n,copy/n)
artifact=copy/'parser.huab';invoke([probe,'build',copy/'parser_main.hua','-o',artifact])
raw=artifact.read_bytes();assert raw[:8]==bytes.fromhex(contract['huab_header']['magic_hex']);assert struct.unpack_from('<II',raw,8)==(7,1);checks+=2
for n in modules:(copy/n).unlink()
assert not list(copy.glob('*.hua'));checks+=1
assert invoke([hua,'check',artifact]).stdout=='OK\n'

positive=[
 '', '\n\n', '1 + 2 * 3\n', '8-3-1\n', '2**3**2\n', '-2**2\n', '(-2)**2\n',
 'a=b=1\n', 'a+=2*3\n', 'true || false && !false\n', '1<2 == true\n',
 'f(1,2+3)\n', 'f()(2)\n', '(1+2)*3\n', 'let x=0xAB_ff\nvar y float=1.5e+2\nconst z int=-9223372036854775808\n',
 'fn f(x int,y float) bool {return x<y}\n', 'fn main(){return}\n', 'fn f() void {}\n',
 'if true {} else {}\n', 'if a {1}\nelse if b {2}\nelse {3}\n', 'if true {}\nlet x=1\n',
 'while true {break}\n', 'fn f(x int) int {while x>0 {x-=1\nif x==2{continue}\nif x==1{break}}\nreturn x}\n',
 'fn f(\n x int,\n y float\n) float {return f(\n1,\n2\n)}\n',
 '#* 外层 #* 内层 *#\n*# let x=1\n# 注释中😀\nprint(x)\n', '\ufefflet x=1\r\nif true {print(x)}\r',
 '\tvar x=1\n{x=2}\n', 'let x=1+\n2\n', 'fn f() int {return\n1}\n', 'print(999)\n',
 # Syntax acceptance must not pretend to perform semantic checking.
 'let x int=true\n', 'unknown(1)\n', 'let x=1//0\n',
 # Exact byte ceiling, a wider program, and an expression near the depth ceiling.
 ' '*8191+'1', '1\n'*31+'1', '('*14+'1'+')'*14,
 'fn sum(n int) int {var total=0\nvar i=0\nwhile i<n {total+=i\ni+=1}\nreturn total}\nfn main(){print(sum(5))}\n']
negative=['let =1\n','let x=\n','let x=1 let y=2\n','fn f(x int {return x}\n','if true {1\n','f(1,)\n','(1+2\n','1=2\n','return 1\n','break\n','continue\n','fn f(){while true{fn g(){break}}}\n','1__2\n','0b2\n',';\n','#* unfinished']
# Last nested-function example belongs to the explicit exclusion list instead of oracle diagnostics.
negative.remove('fn f(){while true{fn g(){break}}}\n')
excluded=['import std.fs\n','for i in 0..3{}\n','"hello"\n','nil\n','[1,2]\n','x[0]\n','x.y\n','x++\n','1<<2\n','struct A{x int}\n','enum A{B}\n','interface A{}\n','pub fn f(){}\n','fn f<T>(x T) T{return x}\n','f<int>(1)\n','x<y>(1)\n','fn A.f(){}\n','fn f(x){}\n','fn f(mut x int){}\n','fn f(){fn g(){}}\n','let a,b=1,2\n','fn f(){return 1,2}\n','fn f()(int,int){return 1,2}\n','let x []int=[1]\n','let x int?=nil\n','defer f()\n','async fn f(){}\n','unsafe{}\n','match true{true=>1}\n']

def diagnostic(report):
 m=re.match(r'(E\d+)@(\d+):(\d+)',report['error']);assert m,report
 return m[1],int(m[2]),int(m[3])
def projected(report):
 return [{k:n[k] for k in ['kind','text','start','end','line','column','depth']}|{'arity':len(n['children'])} for n in report['nodes']]

def execute(name,text,phase):
 global checks
 p=work/(name+'.hua');p.write_text(text,encoding='utf-8',newline='')
 reference=json.loads(invoke([oracle,p]).stdout)
 results=[]
 for engine,exe,target in [('interpret',hua,entry),('run',hua,entry),('run',hua,artifact),('run',probe,entry)]:
  result=json.loads(invoke([exe,engine,target,'--',p,'input.hua']).stdout);results.append(result)
  if phase=='positive':
   assert reference['status']==result['status']=='ok',(name,reference,result)
   assert projected(result)==reference['nodes'],(name,projected(result),reference['nodes'])
  else:
   assert result['status']=='error' and 'nodes' not in result,(name,result)
   code,line,column=diagnostic(result)
   if phase=='negative':assert (code,line,column)==(reference['code'],reference['line'],reference['column']),(name,result,reference)
   else:assert code=='E8201',(name,result)
  checks+=1
 assert all(r==results[0] for r in results),name;checks+=1
 rows.append({'case':name,'phase':phase,'AST_VM_source_free_HUAB_SB1':'pass','oracle':'AST/span match' if phase=='positive' else 'code/line/column match' if phase=='negative' else 'explicit subset rejection','result':results[0]})
for group,data in [('positive',positive),('negative',negative),('excluded',excluded)]:
 for i,text in enumerate(data):execute(group+str(i),text,group)

# Limits are measured independently of interpreter/VM instruction counters and can only be reduced.
limits=contract['budgets']
for name,text,overrides in [
 ('source','123\n',{'source_bytes':3}),('tokens','1+2\n',{'tokens':2}),('nodes','1+2\n',{'nodes':3}),
 ('nesting','((1))\n',{'nesting':3}),('work','1\n',{'work':1}),('invalid','1\n',{'nodes':0}),('larger','1\n',{'tokens':513})]:
 d=work/('budget-'+name);d.mkdir(exist_ok=True)
 for n in modules:shutil.copy2(repo/'selfhost'/n,d/n)
 values=limits|overrides;fields=','.join(k+':'+str(v) for k,v in values.items())
 literal=json.dumps(text,ensure_ascii=False)
 source=f'''import parser\nfn main(){{let result=parser.parse_with_limits({literal},"input.hua",parser.Limits{{{fields}}})\nprint(unwrap_err(result))}}\n'''
 path=d/'main.hua';path.write_text(source,encoding='utf-8')
 invoke([probe,'check',path])
 expected=None
 for engine in ('interpret','run'):
  result=invoke([hua,engine,path]).stdout
  assert result.startswith('E8202@'),(name,result)
  if expected is None:expected=result
  else:assert result==expected
  checks+=1
 huab=d/'limits.huab';invoke([probe,'build',path,'-o',huab])
 for file in d.glob('*.hua'):file.unlink()
 assert invoke([hua,'run',huab]).stdout==expected;checks+=1
 rows.append({'case':'budget-'+name,'result':expected.strip(),'all_paths':'pass'})

# Development runner: exit codes, no input execution, byte determinism, atomic failure preservation.
runner=repo/'tools/hua_frontend.py';source=work/'command.hua';target=work/'output.json'
for args in [[],['parse'],['unknown','input.hua'],['parse','input.hua','--unknown']]:
 r=invoke([sys.executable,runner,*args],2)
 error=json.loads(r.stderr);assert error['code']=='E8204' and error['stage']=='command' and r.stdout=='';checks+=1
def command(text,expected_code=0):
 source.write_text(text,encoding='utf-8',newline='');return invoke([sys.executable,runner,'parse',source,'--hua',hua,'-o',target],expected_code)
command('let x=1\n');first=target.read_bytes();command('let x=1\n');assert target.read_bytes()==first;checks+=1
other=work/'relocated';other.mkdir(exist_ok=True);p=other/source.name;p.write_bytes(source.read_bytes())
r=invoke([sys.executable,runner,'parse',p,'--hua',hua]);assert r.stdout.encode()==first;checks+=1
for bad in ['let x=\n','print(987654)\nimport std.fs\n','1 '*513,' '*8193,'('*15+'1'+')'*15]:
 r=command(bad,1);assert r.stdout=='' and target.read_bytes()==first and 'nodes' not in json.loads(r.stderr);checks+=1
source.write_bytes(b'\xff');r=invoke([sys.executable,runner,'parse',source,'--hua',hua,'-o',target],2);assert target.read_bytes()==first;checks+=1
invoke([sys.executable,runner,'parse',work/'missing.hua','--hua',hua,'-o',target],2)
invoke([sys.executable,runner,'parse',source,'--hua',hua,'-o',work/'wrong.huab'],2)
assert not (work/'wrong.huab').exists();checks+=1
source.write_text('1\n',encoding='utf-8');invoke([sys.executable,runner,'parse',source,'--hua',hua,'-o',work/'missing-dir/result.json'],2)
assert not list(work.glob('output.json.tmp-*'));checks+=1
# Inject malformed host responses without adding production hooks.
spec=importlib.util.spec_from_file_location('frontend',runner);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
valid_report=json.loads(first);malformed=json.loads(first);malformed['nodes'][0]['children']=[999]
for reply in ['not json',json.dumps(malformed),json.dumps({'status':'ok'})]:
 fake=subprocess.CompletedProcess([],0,reply,'')
 with patch.object(module.subprocess,'run',return_value=fake),redirect_stderr(io.StringIO()),redirect_stdout(io.StringIO()):
  code=module.main(['parse',str(source),'--hua',str(hua),'-o',str(target)])
 assert code==3 and target.read_bytes()==first;checks+=1
with patch.object(module.os,'replace',side_effect=OSError('injected replace failure')),redirect_stderr(io.StringIO()),redirect_stdout(io.StringIO()):
 code=module.main(['parse',str(source),'--hua',str(hua),'-o',str(target)])
assert code==2 and target.read_bytes()==first and not list(work.glob('output.json.tmp-*'));checks+=1

# A profile ceiling does not enlarge the host's separate execution limit.
source.write_text('1\n'*255+'1',encoding='utf-8',newline='')
r=invoke([sys.executable,runner,'parse',source,'--hua',hua,'-o',target],1)
assert json.loads(r.stderr)['code']=='E4099' and target.read_bytes()==first and r.stdout=='';checks+=1

# C01 HUAB evidence is from the existing host; a Hua bytecode encoder is still pending.
h=work/'hc0.hua';h.write_text('fn main(){print(3//2)}\n',encoding='utf-8')
archives=[]
for n in ('a','b'):
 p=work/(n+'.huab');invoke([hua,'build',h,'-o',p]);archives.append(p.read_bytes())
assert archives[0]==archives[1];checks+=1
image=archives[0];assert struct.unpack_from('<IIQII',image,8)==(6,1,len(image)-32,zlib.crc32(image[32:]),0);checks+=1
h.unlink();assert invoke([hua,'run',work/'a.huab']).stdout=='1\n';checks+=1
for name,offset,data in [('crc',24,b'\0\0\0\0'),('flags',28,b'\x01\0\0\0'),('abi',12,b'\x02\0\0\0'),('version',8,b'\x09\0\0\0')]:
 corrupt=bytearray(image);corrupt[offset:offset+len(data)]=data;p=work/(name+'.huab');p.write_bytes(corrupt)
 r=invoke([hua,'run',p],1);assert '[E6002]' in r.stderr and r.stdout=='';checks+=1
report={'result':'passed','contract':'HC-CONTRACT-1','profile':'C02-P1','platform':sys.platform,'cases':len(rows),'checks':checks,'positive':len(positive),'negative':len(negative),'exclusions':len(excluded),'budgets':limits,'publication_exit_determinism':'passed','host_HUAB6_and_parser_HUAB7':'passed','host_scale_limit':{'tokens':512,'flat_expressions':256,'code':'E4099','output_preserved':True},'Hua_encoder_implemented':False,'rows':rows}
(out/'parser-results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(f'C01/C02: {len(rows)} cases, {checks} checks; AST/diagnostics/budgets/publication/HUAB passed')
