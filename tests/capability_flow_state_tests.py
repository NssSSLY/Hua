"""Exact sparse local-flow state: immutable marks, nested restores and joins."""
from pathlib import Path
import argparse,re,sys,json,shutil,time
from capability_metadata_tests import source,extract
from capability_fixture_support import adapt_fixture
sys.path.insert(0,str(Path(__file__).resolve().parent/'tools'))
from audit_bootstrap_phases import command,save

def fixture(cap):
 text=cap.read_text(encoding='utf-8');code=source(cap,1).split('fn main(){',1)[0]
 for name in ('mark','saved','finish','restore','join_branches','join_saved','join'):
  if not re.search(r'^fn '+name+r'\(',code,re.M):code+='\n'+extract(text,name)
 code+='''
fn check(b bool,label string){if !b{print("FAIL",label)}else{print("ok",label)}}
fn main(){
var initial=map[string]Value{}
for i in 0..80{initial["x"+str(i)]=value("[]int",true)}
var e=environment(0,"test",initial)
let first=mark(e)
store(e,"x0",value("[]int",false))
let first_version=unwrap(e.versions["x0"])
let a=saved(e,first)
check(len(a.touched)==1 && len(a.before)==1 && len(a.after)==1,"sparse one-of-80")
store(e,"x1",value("[]int",false))
check(len(a.touched)==1 && !has(a.before,"x1") && !has(a.after,"x1"),"saved mark is immutable")
let all=finish(e,first)
restore(e,all)
check(unwrap(e.values["x0"]).root.writable && unwrap(e.values["x1"]).root.writable,"restore values")
store(e,"x0",value("[]int",false))
check(unwrap(e.versions["x0"])>first_version,"monotonic allocator")
let outer=mark(e)
store(e,"x0",value("[]int",true))
let inner=mark(e)
store(e,"x0",value("[]int",false))
store(e,"fresh",value("int",true))
let child=finish(e,inner)
restore(e,child)
check(unwrap(e.values["x0"]).root.writable && !has(e.values,"fresh"),"nested restore/presence")
let parent=finish(e,outer)
restore(e,parent)
check(!unwrap(e.values["x0"]).root.writable && !has(e.values,"fresh"),"outer first-write fact")
store(e,"x0",value("[]int",true))
let ym=mark(e)
store(e,"x0",value("[]int",false))
store(e,"yes_only",value("int",true))
let yes=finish(e,ym)
restore(e,yes)
let nm=mark(e)
store(e,"x1",value("[]int",false))
store(e,"no_only",value("int",true))
let no=finish(e,nm)
join_branches(e,yes,no)
check(!unwrap(e.values["x0"]).root.writable && !unwrap(e.values["x1"]).root.writable,"disjoint branch writes meet")
check(!has(e.values,"yes_only") && has(e.values,"no_only"),"branch declaration presence")
check(unwrap(e.values["x79"]).root.writable && !has(e.versions,"x79"),"untouched fact/version")
store(e,"x0",value("[]int",true))
store(e,"x1",value("[]int",true))
let match_mark=mark(e)
var merged=saved(e,match_mark)
for i in 0..2{
 restore(e,merged)
 let arm=mark(e)
 store(e,"x"+str(i),value("[]int",false))
 let next=finish(e,arm)
 join_branches(e,merged,next)
 merged=saved(e,match_mark)
}
finish(e,match_mark)
check(!unwrap(e.values["x0"]).root.writable && !unwrap(e.values["x1"]).root.writable,"match accumulates earlier arms")
store(e,"x0",value("[]int",true))
let loop=mark(e)
let iteration=mark(e)
store(e,"x0",value("[]int",false))
join_saved(e,loop)
join_saved(e,iteration)
let d=finish(e,iteration)
finish(e,loop)
check(len(d.before)==1 && !same_value(unwrap(d.before["x0"]),unwrap(e.values["x0"])),"loop changed facts only")
var box=value("test#Box",true)
attach(box,"#@a",value("int",true),false)
store(e,"box",box)
let batch=mark(e)
begin_updates(e)
put(e,"box#@a",value("int",false),false,false)
put(e,"box#@b",value("int",true),false,false)
end_updates(e)
let writes=finish(e,batch)
restore(e,writes)
check(same_value(unwrap(e.values["box"]),box),"batched write first-before isolation")
check(len(e.journals)==0 && !e.updating && len(e.alias_cache)==0,"cleanup")
}
'''
 return adapt_fixture(code,text)

def main():
 p=argparse.ArgumentParser();p.add_argument('exe',type=Path);p.add_argument('root',type=Path);p.add_argument('out',type=Path);p.add_argument('--source',type=Path);a=p.parse_args();r=a.root.resolve();o=a.out.resolve();o=o/('run-'+str(time.time_ns())) if o.exists() else o;o.mkdir(parents=True,exist_ok=False);shutil.copyfile(r/'selfhost/type_rules.hua',o/'type_rules.hua');src=o/'flow.hua';src.write_text(fixture((a.source or r/'tests/experimental/c04-r1-r2/capabilities.hua').resolve()),encoding='utf-8',newline='\n');artifact=src.with_suffix('.huab');outputs=[]
 for mode in ('interpret','run','build'):
  _,out,_=command(r,o,mode,[a.exe.resolve(),mode,src]+(['-o',artifact] if mode=='build' else []),60)
  if mode!='build':outputs.append(out)
 src.rename(src.with_suffix('.hidden'))
 _,out,_=command(r,o,'source-free',[a.exe.resolve(),'run',artifact],30);outputs.append(out)
 assert all(x==outputs[0] for x in outputs) and b'FAIL' not in out and len(out.splitlines())==13,out
 save(o/'result.json',{'status':'passed','checks':13,'paths':3,'output':out.decode().splitlines()});print(out.decode())
if __name__=='__main__':main()
