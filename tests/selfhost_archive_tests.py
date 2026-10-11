# C07: complete Hua encoding, independent native writer/reader and safe publication.
from pathlib import Path
import ast,subprocess,sys,json,struct,shutil,re,importlib.util,io,copy,zlib,time
from unittest.mock import patch
from contextlib import redirect_stderr,redirect_stdout
hua,probe,driver,repo,build=map(lambda p:Path(p).resolve(),sys.argv[1:6])
smoke=len(sys.argv)>6 and sys.argv[6]=='--smoke'
out=build/'c07';out.mkdir(exist_ok=True);work=out/'acceptance';work.mkdir(exist_ok=True)
entry=repo/'selfhost/archive_main.hua';records=[];checks=0;started=time.monotonic()
def invoke(args,code=0,timeout=600):
 p=subprocess.run(list(map(str,args)),capture_output=True,encoding='utf-8',timeout=timeout)
 assert p.returncode==code,(args,p.returncode,p.stdout[:1500],p.stderr[:4000])
 return p
spec=importlib.util.spec_from_file_location('runner',repo/'tools/hua_frontend.py');runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
paths=[(hua,'run',entry)]
if not smoke:
 persisted=work/'persisted';persisted.mkdir(exist_ok=True)
 for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,persisted/p.name)
 invoke([probe,'check',entry]);component=persisted/'archive-component.huab';invoke([probe,'build',persisted/entry.name,'-o',component])
 assert struct.unpack_from('<II',component.read_bytes(),8)==(7,1)
 assert persisted.parent.resolve()==work.resolve()
 for p in persisted.glob('*.hua'):p.unlink()
 assert not list(persisted.glob('*.hua'))
 paths=[(hua,'interpret',entry),(hua,'run',entry),(hua,'run',component),(probe,'run',entry)];checks+=3
fixtures={}
for n in ast.parse((repo/'tests/selfhost_lowering_tests.py').read_text(encoding='utf-8')).body:
 if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name) and n.targets[0].id in ('positive','runtime_errors','negative'):fixtures[n.targets[0].id]=ast.literal_eval(n.value)
positive=fixtures['positive'];errors=fixtures['runtime_errors'];negative=fixtures['negative']
positive.update({
 'int_limits':('fn main(){print(-9223372036854775808,9223372036854775807,0x7f,0b101,0o17,1_234)}\n',None),
 'float_bits':('fn main(){print(0.0,-0.0,0.1,1.2345678901234567,1e3,-1.5,2.2250738585072014e-308,1.7976931348623157e308)}\n',None),
 'void_gate':('fn f() Result<void>{return ok()}\nfn main(){print(is_ok(f()))}\n','true\n'),
 'raw_array':('fn f(x [0_2]int) int{return x[1]}\nfn main(){print(f([1,2]))}\n','2\n'),
 'init_order':('print("init")\nfn main(){print("main")}\n','init\nmain\n')})
receipts={}
def case(label,text,expected=None,runtime=False,files=None,filename='input.hua',reject=None):
 global checks
 project=work/label;project.mkdir(exist_ok=True);source=project/filename;source.write_text(text,encoding='utf-8',newline='')
 for name,body in (files or {}).items():
  p=project/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body,encoding='utf-8',newline='\n')
 results=[];artifacts=[]
 for i,(exe,action,target) in enumerate(paths):
  artifact=project/f'path-{i}.huab';artifact.write_bytes(b'KEEP')
  p=invoke([exe,action,target,'--execution-profile','compiler','--report-budget','--',source,artifact])
  receipt=json.loads(p.stdout);metrics=json.loads(p.stderr);assert metrics['profile']=='compiler' and metrics['steps']<=500000000 and metrics['peak_calls']<=128
  assert receipt['schema']=='hua.archive.result.v1' and receipt['profile']=='C07-HUAB1'
  results.append(receipt);artifacts.append(artifact)
  if reject:
   assert receipt['status']=='error' and receipt['error'].startswith(reject+'@'),(label,receipt)
   assert artifact.read_bytes()==b'KEEP' and not any(k in receipt for k in ('file_bytes','crc32','instructions'))
  else:
   assert receipt['status']=='ok',(label,receipt)
   data=artifact.read_bytes();assert data[:8]==b'HUAB\r\n\x1a\n' and len(data)==receipt['file_bytes']
   assert struct.unpack_from('<IIQII',data,8)==(receipt['format'],1,len(data)-32,zlib.crc32(data[32:]),0)
   invoke([hua,'check',artifact]);checks+=3
 assert all(r==results[0] for r in results),label
 assert all(p.read_bytes()==artifacts[0].read_bytes() for p in artifacts),label
 assert not list(project.glob('hua-huab-*'))
 if reject:
  with redirect_stderr(io.StringIO()):assert runner.main(['build',str(source),'--hua',str(hua),'-o',str(artifacts[0])])==1
  assert artifacts[0].read_bytes()==b'KEEP';checks+=4;records.append({'case':label,'status':'rejected'});return
 baseline=[invoke([hua,a,source],1 if runtime else 0) for a in ('interpret','run')];assert baseline[0].stdout==baseline[1].stdout
 if expected is not None:assert baseline[0].stdout==expected,(label,baseline[0].stdout,expected)
 report=json.loads(invoke([hua,'run',repo/'selfhost/bytecode_main.hua','--execution-profile','compiler','--',source]).stdout)
 target=project/'image.json';target.write_text(json.dumps(report,ensure_ascii=False),encoding='utf-8')
 native=project/'native-reference.huab';invoke([driver,'build',target,'-o',native]);assert native.read_bytes()==artifacts[0].read_bytes(),(label,'Hua/native archive bytes differ')
 receipts[label]=results[0]
 assert project.resolve().parent==work.resolve()
 for p in project.rglob('*.hua'):p.unlink()
 assert not list(project.rglob('*.hua'))
 for artifact in artifacts:
  p=invoke([hua,'run',artifact],1 if runtime else 0);assert p.stdout==baseline[0].stdout,(label,p.stdout,baseline[0].stdout)
  if runtime:
   ec=re.search(r'error\[(E\d+)\]',baseline[0].stderr)[1];assert 'error['+ec+']' in p.stderr
   pos=re.search(r':(\d+):(\d+)',baseline[0].stderr);assert f'{filename}:{pos[1]}:{pos[2]}' in p.stderr
 checks+=5+len(paths);records.append({'case':label,'status':'ok','format':results[0]['format'],'bytes':results[0]['file_bytes']})
for label,(text,expected) in positive.items():case(label,text,expected)
for label,value in errors.items():case(label,value if isinstance(value,str) else value[0],runtime=True)
for label,(text,ec) in negative.items():case(label,text,reject=ec)
case('modules','import dep as d\nfn main(){print(d.f(3))}\n','dep\n4\n',files={'dep.hua':'print("dep")\npub fn f(x int) int{return x+1}\n'})
case('module_enum','import dep as d\nfn main(){print(d.f(d.E.A(3)))}\n','3\n',files={'dep.hua':'pub enum E{A(int),B}\npub fn f(x E) int{match x{A(v)=>{return v}\nB=>{return 0}}}\n'})
case('unicode','struct A{x string}\nfn main(){print(A{x:"中文\\0🙂"}.x)}\n','中文\0🙂\n',filename='中文 #空格$.hua')
assert receipts['void_gate']['format']==7 and receipts['arithmetic']['format']==6
# Relocation, newline normalization and deterministic logical source/declaration order.
relocations=[]
for label,text in [('root-a','\ufeffstruct Z{x int}\r\nstruct A{x int}\r\nfn z()int{return 1}\r\nfn main(){print(z())}\r\n'),('root-b','struct Z{x int}\nstruct A{x int}\nfn z()int{return 1}\nfn main(){print(z())}\n')]:
 project=work/label;project.mkdir(exist_ok=True);source=project/'same.hua';source.write_text(text,encoding='utf-8',newline='')
 target=project/'out.huab'
 with redirect_stdout(io.StringIO()):assert runner.main(['build',str(source),'--hua',str(hua),'-o',str(target)])==0
 relocations.append(target.read_bytes())
assert relocations[0]==relocations[1];checks+=3
# Float subnormals are tested through the public bytecode image encoder directly,
# including the smallest value and the normal/subnormal boundary.
project=work/'subnormals';project.mkdir(exist_ok=True);source=project/'input.hua'
source.write_text('fn main(){print(4.9406564584124654e-324,-4.9406564584124654e-324,2.2250738585072009e-308,1e-300)}\n',encoding='utf-8')
p=invoke([hua,'run',entry,'--execution-profile','compiler','--',source,project/'out.huab']);receipt=json.loads(p.stdout)
if receipt['status']=='ok':
 image=json.loads(invoke([hua,'run',repo/'selfhost/bytecode_main.hua','--execution-profile','compiler','--',source]).stdout)
 (project/'image.json').write_text(json.dumps(image),encoding='utf-8');invoke([driver,'build',project/'image.json','-o',project/'native.huab']);assert (project/'native.huab').read_bytes()==(project/'out.huab').read_bytes()
else:raise AssertionError(('subnormals not supported by existing numeric decode',receipt))
checks+=2
# Invalid artifacts with recalculated CRC still fail before initialization.
raw=(work/'init_order/path-0.huab').read_bytes();corruptions={}
for label,offset,value in [('magic',0,0),('version',8,99),('abi',12,2),('flags',28,1)]:
 b=bytearray(raw);b[offset]=value;corruptions[label]=bytes(b)
b=bytearray(raw);b[24]^=1;corruptions['crc']=bytes(b)
corruptions['truncated']=raw[:-1]
b=bytearray(raw);b.extend(b'X');struct.pack_into('<Q',b,16,len(b)-32);struct.pack_into('<I',b,24,zlib.crc32(b[32:]));corruptions['trailing']=bytes(b)
b=bytearray((work/'void_gate/path-0.huab').read_bytes());struct.pack_into('<I',b,8,6);corruptions['void_downgrade']=bytes(b)
for label,data in corruptions.items():
 p=work/(label+'.huab');p.write_bytes(data)
 for action in ('check','run'):
  result=invoke([hua,action,p],1);assert '[E6002]' in result.stderr and result.stdout==''
 checks+=2
# Real publication validates without executing initialization and preserves old output on failure.
project=work/'publication';project.mkdir(exist_ok=True);source=project/'input.hua';target=project/'keep.huab';marker=project/'effect.txt';marker.unlink(missing_ok=True)
source.write_text('import std.fs as fs\nlet done=fs.write_text('+json.dumps(marker.as_posix())+',"touched")\nfn main(){print(unwrap(done))}\n',encoding='utf-8')
target.write_bytes(b'KEEP')
with redirect_stdout(io.StringIO()):assert runner.main(['build',str(source),'--hua',str(hua),'-o',str(target)])==0
assert not marker.exists();kept=target.read_bytes()
source.write_text('fn main(){let n int="wrong"}\n',encoding='utf-8')
with redirect_stderr(io.StringIO()):assert runner.main(['build',str(source),'--hua',str(hua),'-o',str(target)])==1
assert target.read_bytes()==kept and not list(project.glob('*.tmp-*')) and not list(project.glob('hua-huab-*'))
source.write_text('fn main(){print(1)}\n',encoding='utf-8')
for args in [['build',str(source),'--hua',str(hua)],['build',str(source),'--hua',str(hua),'-o',str(project/'bad.json')],['build',str(source),'--hua',str(hua),'-o',str(target),'--budget','slice']]:
 with redirect_stderr(io.StringIO()):assert runner.main(args)==2
 assert target.read_bytes()==kept
# A real Python command writes UTF-8 JSON even when Windows defaults to ANSI.
source=project/'中文🙂.hua';source.write_text('fn main(){print(7)}\n',encoding='utf-8')
p=invoke([sys.executable,repo/'tools/hua_frontend.py','build',source,'--hua',hua,'-o',target])
assert json.loads(p.stdout)['source']==source.name and invoke([hua,'run',target]).stdout=='7\n'
source.write_text('fn main(){let x int="wrong"}\n',encoding='utf-8');prior=target.read_bytes()
p=invoke([sys.executable,repo/'tools/hua_frontend.py','build',source,'--hua',hua,'-o',target],1)
assert json.loads(p.stderr)['source']==source.name and target.read_bytes()==prior
missing=project/'缺少🙂.hua'
p=invoke([sys.executable,repo/'tools/hua_frontend.py','build',missing,'--hua',hua,'-o',target],2)
assert json.loads(p.stderr)['code']=='E8204' and target.read_bytes()==prior
source=project/'input.hua';checks+=6
# Deliberately broken stage bytes/receipt/native-reader result must not reach target.
def stage(args,**kwargs):
 shutil.copy2(work/'arithmetic/path-0.huab',args[-1]);return subprocess.CompletedProcess(args,0,json.dumps(receipts['arithmetic']), '')
for label in ('invalid_envelope','missing_counter','boolean_counter','crc','reader','timeout','replacement'):
 def run(args,**kwargs):
  if args[1]=='check':return subprocess.CompletedProcess(args,1 if label=='reader' else 0,'OK\n','rejected' if label=='reader' else '')
  if label=='timeout':raise subprocess.TimeoutExpired(args,600)
  result=stage(args,**kwargs);v=json.loads(result.stdout)
  if label=='invalid_envelope':v['schema']='wrong'
  if label=='missing_counter':v.pop('file_bytes')
  if label=='boolean_counter':v['crc32']=True
  if label=='crc':p=Path(args[-1]);b=bytearray(p.read_bytes());b[-1]^=1;p.write_bytes(b)
  result.stdout=json.dumps(v);return result
 target.write_bytes(kept)
 with patch.object(runner.subprocess,'run',side_effect=run),redirect_stderr(io.StringIO()),redirect_stdout(io.StringIO()):
  if label=='replacement':
   with patch.object(runner.os,'replace',side_effect=OSError('replacement denied')):status=runner.main(['build',str(source),'--hua',str(hua),'-o',str(target)])
  else:status=runner.main(['build',str(source),'--hua',str(hua),'-o',str(target)])
 assert status==(1 if label=='timeout' else 2 if label=='replacement' else 3),(label,status)
 assert target.read_bytes()==kept and not list(project.glob('*.tmp-*'))
 checks+=2
checks+=9
result={'result':'passed','cases':len(records),'checks':checks,'paths':len(paths),'corrupt_artifacts':len(corruptions),'publication_failures':7,'relocation_bytes_identical':True,'float_subnormal_bytes_identical':True,'seconds':round(time.monotonic()-started,3),'rows':records}
(out/('archive-smoke-results.json' if smoke else 'archive-results.json')).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps({k:v for k,v in result.items() if k!='rows'}))
