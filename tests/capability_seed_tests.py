"""Exact parameter-origin reuse, write isolation and bounded cache admission."""
from pathlib import Path
import argparse,re,json,os,sys,time
from process_guard import run
from capability_fixture_support import adapt_fixture,logical_record

def extract(text,name,optional=False):
 m=re.search(r'(?ms)^fn '+name+r'\(.*?(?=^(?:pub )?fn |\Z)',text)
 if not m and optional:return ''
 assert m,name
 return m.group()

def source(cap,width):
 text=cap.read_text(encoding='utf-8');decl=text[text.index('pub struct View {'):text.index('# Node consumers')]
 env=text[text.index('struct Environment {'):text.index('fn view(')].replace('unit modules.Unit','unit int')
 names=('view','value','record','split_origin','depth','projected','child_type','note_prefix','note_type','prefix_index','field_shape','field_defaults','seeded','parameter_seeded','copy_view','materialized','copied','merge_view','field_fact','merge','select_root','select','path_owner','path_tail','value_path','lookup_path','commit','store','begin_updates','end_updates','editable','attach','put','same_flags','same_view','same_fields','same_value','snapshot','environment')
 functions='\n'.join(extract(text,n,n=='parameter_seeded') for n in names).replace('unit modules.Unit','unit int')
 state='struct State{members map[string]map[string]string\nshapes map[string]map[string]View\nshape_prefixes map[string]map[string]map[string]bool\nparameter_fields map[string]map[string]View\nparameter_field_count int}\n'
 fields={f'v{i}':'int' for i in range(width)};fields['next']='[]mod#R'
 literal='map[string]string{'+','.join(json.dumps(k)+':'+json.dumps(v) for k,v in fields.items())+'}'
 main='''fn main(){var s=State{members:map[string]map[string]string{"mod#R":FIELDS},shapes:map[string]map[string]View{},shape_prefixes:map[string]map[string]map[string]bool{},parameter_fields:map[string]map[string]View{},parameter_field_count:0}
var checksum=0
var last=value("",false)
print("@@hua.phase:begin:seed")
for i in 0..32{let input=seeded(s,"mod#R",true,"p")
var e=environment(0,"caller",map[string]Value{"a":input,"b":input})
checksum+=len(field_fact(input,"#@v0","int").origins)
put(e,"a#@v0",value("int",true),false,false)
last=unwrap(e.values["a"])
if !same_value(unwrap(e.values["b"]),input){print("MUTATED")}
}
print("@@hua.phase:end:seed")
print(checksum)
print(unwrap(json.encode(materialized(last))))
let ro=seeded(s,"mod#R",false,"borrow")
print(unwrap(json.encode(materialized(select(ro,"#@next","[]mod#R")))))
}'''.replace('FIELDS',literal)
 if re.search(r'^fn parameter_seeded\(',text,re.M):main=main.replace('=seeded(s,','=parameter_seeded(s,')
 return adapt_fixture('import type_rules as rules\nimport std.strings as strings\nimport std.json as json\nimport std.list as list\n'+decl+state+env+functions+main,text)



def main():
 p=argparse.ArgumentParser();p.add_argument('hua',type=Path);p.add_argument('root',type=Path);p.add_argument('output',type=Path);p.add_argument('--source',type=Path);p.add_argument('--tag',default='run-'+str(time.time_ns()));p.add_argument('--reference',type=Path);p.add_argument('--contract-only',action='store_true');a=p.parse_args();R=a.root.resolve();W=a.output.resolve()/a.tag;W.mkdir(parents=True,exist_ok=False);exe=a.hua.resolve();cap=(a.source or R/'selfhost/capabilities.hua').resolve()
 import shutil
 os.environ['HUA_TEST_TIMEOUT_SECONDS']='45'
 os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS']='180'
 shutil.copy2(R/'selfhost/type_rules.hua',W/'type_rules.hua');rows=[]
 def call(label,args):
  before=time.monotonic();r=run([str(exe),*map(str,args)],cwd=R,capture_output=True,timeout=45)
  (W/(label+'.stdout')).write_bytes(r.stdout);(W/(label+'.stderr')).write_bytes(r.stderr)
  assert r.returncode==0,(label,r.returncode,r.stderr[-3000:])
  return r,time.monotonic()-before
 for width in (() if a.contract_only else (8,64,256)):
  src=W/f'seed-{width}.hua';src.write_text(source(cap,width),encoding='utf-8',newline='\n');artifact=src.with_suffix('.huab')
  call(str(width)+'-build',['build',src,'-o',artifact]);r,seconds=call(str(width)+'-run',['run',artifact,'--execution-profile','compiler','--report-budget'])
  lines=[x for x in r.stdout.decode().splitlines() if not x.startswith('@@hua.phase:')];assert len(lines)==3 and lines[0]=='32',lines[:1]
  decoded=[logical_record(json.loads(line)) for line in lines[1:]];budget=json.loads(r.stderr);row={'width':width,'seconds':seconds,'steps':budget['steps'],'logical':decoded};rows.append(row)
  if a.reference:
   prior=json.loads(a.reference.read_text())['rows'];expected=next(v for v in prior if v['width']==width);assert expected['logical']==decoded,(width,'logical capability fields differ')
  print(json.dumps({k:v for k,v in row.items() if k!='logical'}),flush=True)
  if width==8:
   for mode in ['interpret','run']:
    r,_=call('8-'+mode,['interpret' if mode=='interpret' else 'run',src,'--execution-profile','compiler','--report-budget']);other=[x for x in r.stdout.decode().splitlines() if not x.startswith('@@hua.phase:')];assert other==lines
   src.rename(src.with_suffix('.hidden'));r,_=call('8-source-free',['run',artifact,'--execution-profile','compiler','--report-budget']);assert [x for x in r.stdout.decode().splitlines() if not x.startswith('@@hua.phase:')]==lines

 if re.search(r'^fn parameter_seeded\(',cap.read_text(encoding='utf-8'),re.M):
  base=source(cap,8).split('var checksum=0',1)[0]
  initializer=base.rsplit('var s=',1)[1].strip()
  body=r"""
let expected=seeded(s,"mod#R",true,"p")
let first=parameter_seeded(s,"mod#R",true,"p")
var warm=parameter_seeded(s,"mod#R",true,"p")
check(same_value(expected,first) && same_value(first,warm),"cold/warm full origins")
check(len(s.parameter_fields)==1 && s.parameter_field_count==len(expected.fields),"one complete retained table")
var e=environment(0,"test",map[string]Value{"a":warm,"b":warm})
begin_updates(e)
put(e,"a#@v0",value("int",true),false,false)
put(e,"a#@next#@[]#@v0",value("int",false),false,false)
end_updates(e)
check(same_value(unwrap(e.values["b"]),expected),"unmodified alias")
check(!same_value(unwrap(e.values["a"]),expected),"write actually changes value")
check(same_value(parameter_seeded(s,"mod#R",true,"p"),expected),"cached origins survive batched field writes")
let ro=parameter_seeded(s,"mod#R",false,"p")
let other=parameter_seeded(s,"mod#R",true,"q")
check(same_value(ro,seeded(s,"mod#R",false,"p")),"readonly key")
check(same_value(other,seeded(s,"mod#R",true,"q")),"owner key")
let a=parameter_seeded(s,"b:c",false,"a")
let b=parameter_seeded(s,"c",false,"a:b")
check(same_value(a,seeded(s,"b:c",false,"a")) && same_value(b,seeded(s,"c",false,"a:b")),"delimiter identities")
check(same_value(parameter_seeded(s,"b:c",false,"a"),a),"distinct warm delimiter key")
# A new graph with the same name can declare a different type.
var fresh=INITIALIZER
var members=unwrap(fresh.members["mod#R"])
members["v0"]="string"
let renewed=parameter_seeded(fresh,"mod#R",true,"p")
check(unwrap(renewed.fields["#@v0"]).type_name=="string" && !same_value(renewed,expected),"fresh analysis graph")
# Exact entry budget includes the final admitted table; overflow bypasses.
var bounded=INITIALIZER
bounded.parameter_field_count=4096-len(expected.fields)
check(same_value(parameter_seeded(bounded,"mod#R",true,"p"),expected),"last entry admission")
check(bounded.parameter_field_count==4096 && len(bounded.parameter_fields)==1,"entry bound reached")
check(same_value(parameter_seeded(bounded,"mod#R",true,"q"),seeded(bounded,"mod#R",true,"q")),"entry overflow exact bypass")
check(bounded.parameter_field_count==4096 && len(bounded.parameter_fields)==1,"entry overflow not retained")
check(same_value(parameter_seeded(bounded,"mod#R",true,"p"),expected),"existing hit at capacity")
var full=INITIALIZER
for i in 0..4096{full.parameter_fields[str(i)]=map[string]View{}}
check(same_value(parameter_seeded(full,"mod#R",true,"p"),seeded(full,"mod#R",true,"p")),"key capacity exact bypass")
check(len(full.parameter_fields)==4096 && full.parameter_field_count==0,"key capacity not retained")
print("CACHE-CONTRACT-PASSED")
}
""".replace('INITIALIZER',initializer)
  generated=base.rsplit('fn main(){',1)[0]+'fn check(ok bool,label string){if !ok{print("FAILED "+label)}}\n'+base[base.rfind('fn main(){'):]+body
  contract=W/'cache-contract.hua';contract.write_text(adapt_fixture(generated,cap.read_text(encoding='utf-8')),encoding='utf-8',newline='\n');artifact=contract.with_suffix('.huab')
  call('contract-build',['build',contract,'-o',artifact])
  for mode in ['interpret','run']:
   r,_=call('contract-'+mode,[mode,contract,'--execution-profile','compiler','--report-budget']);assert r.stdout.decode().splitlines()==['CACHE-CONTRACT-PASSED'],r.stdout.decode()
  contract.rename(contract.with_suffix('.hidden'))
  r,_=call('contract-source-free',['run',artifact,'--execution-profile','compiler','--report-budget']);assert r.stdout.decode().splitlines()==['CACHE-CONTRACT-PASSED'],r.stdout.decode()
  print('cache contract: 17 assertions; AST/VM/source-free HUAB',flush=True)
 (W/'results.json').write_text(json.dumps({'status':'passed','contract_assertions':17 if 'parameter_seeded' in cap.read_text(encoding='utf-8') else 0,'rows':rows},indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
