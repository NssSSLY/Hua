"""Deferred summary fields preserve snapshots, projections and bounded copy semantics."""
from pathlib import Path
import argparse,re,sys,shutil,json,time
from capability_metadata_tests import source,extract
from capability_fixture_support import adapt_fixture
sys.path.insert(0,str(Path(__file__).resolve().parent/'tools'))
from audit_bootstrap_phases import command,save

def fixture(cap):
 text=cap.read_text(encoding='utf-8');code=source(cap,1).split('fn main(){',1)[0]
 for name in ('instantiate','instantiate_at','instantiate_flat','actuals','summary_actuals'):
  if not re.search(r'^fn '+name+r'\(',code,re.M):code+='\n'+extract(text,name)
 code+='''
fn check(b bool,label string){if !b{print("FAIL",label)}else{print("ok",label)}}
fn main(){
var owner=value("pkg#R",true)
owner.root.origins["old|"]=false
var leaf=value("[]int",false)
leaf.root.origins["borrow|#@child"]=true
attach(owner,"#@slot",leaf,false)
var e=environment(0,"test",map[string]Value{"p":owner})
var source=value("pkg#Wide",true)
source.root.origins["p|"]=false
var formal=value("pkg#R",true)
formal.root.origins["p|#@slot"]=false
for i in 0..64{attach(source,"#@f"+str(i),formal,false)}
attach(source,"#@f0#@child",formal,false)
let snapshot=unwrap(json.encode(source))
let call=instantiate("caller","m",7,source,e.values)
check(call.plan>=4 && len(call.fields)==65,"wide result retains backing table")
check(has(unwrap(call.fields["#@f63"]).origins,"p|#@slot"),"backing field remains formal")
let selected=field_fact(call,"#@f63","pkg#R")
check(has(selected.origins,"borrow|#@child") && !selected.writable,"selected field substitutes actual")
let branch=select(call,"#@f0","pkg#R")
check(branch.plan>=4 && has(field_fact(branch,"#@child","pkg#R").origins,"borrow|#@child"),"nested projection keeps transform")
var replacement=value("[]int",true)
replacement.root.origins["new|"]=false
put(e,"p#@slot",replacement,false,false)
check(has(field_fact(call,"#@f63","pkg#R").origins,"borrow|#@child"),"pre-call actual survives later write")
let newer=instantiate("caller","m",8,source,e.values)
check(has(field_fact(newer,"#@f63","pkg#R").origins,"new|"),"new call observes changed actual")
var copied_once=copied(call)
var repeated=copied_once
for i in 0..10{repeated=copied(repeated)}
check(same_value(copied_once,repeated),"copy-policy bound keeps exact facts")
check(snapshot==unwrap(json.encode(source)),"source remains immutable")
var destination=value("pkg#Outer",true)
attach(destination,"#@result",call,true)
check(has(field_fact(destination,"#@result#@f63","pkg#R").origins,"borrow|#@child"),"attached result resolves transform")
let prepared=summary_actuals(e.values,map[string]bool{"p":true,"missing":true})
check(len(prepared)==1 && has(prepared,"p"),"only referenced actuals retained")
for i in 0..260 { instantiate("caller","m",9,source,e.values) }
check(len(plans)==256,"pool admission is bounded")
let fallback=instantiate("caller","m",10,source,e.values)
check(fallback.plan==0 && has(field_fact(fallback,"#@f63","pkg#R").origins,"new|"),"exact eager admission fallback")
let published=canonical_value(call)
plans=map[int]Transform{}
check(published.plan==0 && has(field_fact(published,"#@f63","pkg#R").origins,"borrow|#@child"),"published value survives pool retirement")
}
'''
 return adapt_fixture(code,text)

def main():
 p=argparse.ArgumentParser();p.add_argument('exe',type=Path);p.add_argument('root',type=Path);p.add_argument('out',type=Path);p.add_argument('--source',type=Path);a=p.parse_args();r=a.root.resolve();o=a.out.resolve();o=o/('run-'+str(time.time_ns())) if o.exists() else o;o.mkdir(parents=True,exist_ok=False);shutil.copyfile(r/'selfhost/type_rules.hua',o/'type_rules.hua');src=o/'transform.hua';src.write_text(fixture((a.source or r/'tests/experimental/c04-r1-r2/capabilities.hua').resolve()),encoding='utf-8',newline='\n');artifact=src.with_suffix('.huab');outputs=[]
 for mode in ('interpret','run','build'):
  _,out,_=command(r,o,mode,[a.exe.resolve(),mode,src]+(['-o',artifact] if mode=='build' else []),60)
  if mode!='build':outputs.append(out)
 src.rename(src.with_suffix('.hidden'));_,out,_=command(r,o,'source-free',[a.exe.resolve(),'run',artifact],30);outputs.append(out)
 assert all(x==outputs[0] for x in outputs) and b'FAIL' not in out and len(out.splitlines())==13,out
 save(o/'result.json',{'status':'passed','checks':13,'paths':3,'output':out.decode().splitlines()});print(out.decode())
if __name__=='__main__':main()
