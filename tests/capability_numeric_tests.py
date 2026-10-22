"""Numeric shape queries against the frozen eager representation; no own-source run."""
from pathlib import Path
import argparse,json,re,shutil,time
from capability_seed_tests import source,extract
from capability_fixture_support import adapt_fixture
from process_guard import run

BODY = r"""
var checks=0
let input=seeded(s,"mod#R",true,"p")
let eager=eager_seeded(s,"mod#R",true,"p")
check(len(input.fields)==0 && len(input.numeric)==1,"numeric backing, no eager map")
check(len(input.numeric[0].facts)>0,"numeric facts exist")
for path, fact in eager.fields { check(same_view(field_fact(input,path,""),fact),"direct field "+path)
checks+=1 }
check(same_value(input,eager),"exact numeric/eager equality")
check(same_value(materialized(input),eager),"explicit boundary equality")
let p=parameter_seeded(s,"mod#R",true,"p")
let q=parameter_seeded(s,"mod#R",true,"q")
check(len(s.numeric_shapes)==1,"different owners reuse one shape")
check(!same_view(field_fact(p,"#@v0","int"),field_fact(q,"#@v0","int")),"owner isolation")
check(len(s.parameter_fields)==0 && s.parameter_field_count==0,"no owner-wide cache")
let before=unwrap(json.encode(input))
var e=environment(0,"test",map[string]Value{"a":input,"b":input})
var replacement=value("int",false)
replacement.root.origins["readonly|"]=true
begin_updates(e)
put(e,"a#@v0",replacement,false,false)
put(e,"a#@new",replacement,false,false)
end_updates(e)
check(!field_fact(unwrap(e.values["a"]),"#@v0","int").writable,"numeric override")
check(field_exists(unwrap(e.values["a"]),"#@new"),"new sparse path")
check(!field_exists(unwrap(e.values["b"]),"#@new"),"sibling isolation")
check(before==unwrap(json.encode(input)),"old snapshot")
check(same_value(copied(input),eager_copied(eager)),"record copy")
let a=select(input,"#@next","[]mod#R")
let b=select(eager,"#@next","[]mod#R")
check(same_value(a,b),"cursor")
check(same_value(select(a,"#@[]","mod#R"),select(b,"#@[]","mod#R")),"nested cursor")
check(same_value(merge(input,unwrap(e.values["a"])),merge(eager,materialized(unwrap(e.values["a"])))),"sparse merge")
var actual=map[string]Value{"p":q}
check(same_value(instantiate_at("site",input,actual),instantiate_at("site",eager,actual)),"numeric substitution")
var ro=seeded(s,"mod#R",false,"ro")
var old_ro=eager_seeded(s,"mod#R",false,"ro")
check(same_value(ro,old_ro) && same_value(copied(ro),eager_copied(old_ro)),"readonly seed/copy")
var numeric_env=environment(0,"alias",map[string]Value{"a":input})
var eager_env=environment(0,"alias",map[string]Value{"a":eager})
let container=field_fact(input,"#@next","[]mod#R")
check(unwrap(json.encode(unwrap(storage_aliases(numeric_env,container))))==unwrap(json.encode(unwrap(storage_aliases(eager_env,container)))),"ordered aliases")
put(numeric_env,"a#@next",value("[]mod#R",true),false,false)
put(eager_env,"a#@next",value("[]mod#R",true),false,false)
check(same_value(unwrap(numeric_env.values["a"]),unwrap(eager_env.values["a"])),"subtree masks")
check(unwrap(json.encode(unwrap(storage_aliases(numeric_env,container))))==unwrap(json.encode(unwrap(storage_aliases(eager_env,container)))),"masked aliases")

var policy=input
var old_policy=eager
for i in 0..12 {
    policy=copied(policy)
    old_policy=eager_copied(old_policy)
    if i%2==0 { policy.root.writable=false
        old_policy.root.writable=false }
    policy=select(policy,"#@next","[]mod#R")
    old_policy=materialized(select(old_policy,"#@next","[]mod#R"))
    check(same_value(policy,old_policy),"copy/readonly composition")
    checks+=1
    if i%2==0 {
        policy=select(policy,"#@[]","mod#R")
        old_policy=materialized(select(old_policy,"#@[]","mod#R")) }
}
var attached=value("mod#Holder",true)
var old_attached=value("mod#Holder",true)
attach(attached,"#@child",ro,true)
attach(old_attached,"#@child",old_ro,true)
check(same_value(attached,old_attached),"readonly copy attach")
print("NUMERIC-PASSED",checks+21)
}
"""


def fixture(cap,reference):
    text=cap.read_text(encoding='utf-8')
    old=reference.read_text(encoding='utf-8')
    assert extract(text,'field_shape')==extract(old,'field_shape'),'shape rules changed: require an independent oracle update'
    code=source(cap,8).split('var checksum=0',1)[0]
    oracle=extract(old,'seeded').replace('fn seeded(','fn eager_seeded(',1)+'\n'+extract(old,'copied').replace('fn copied(','fn eager_copied(',1)
    at=code.index('fn main(')
    code=code[:at]+oracle+code[at:]+BODY
    code=code.replace('fn main(){','fn check(ok bool,label string){if !ok{print("FAILED "+label)}}\nfn main(){',1)
    return adapt_fixture(code,text)

def main():
    p=argparse.ArgumentParser()
    p.add_argument('exe',type=Path);p.add_argument('root',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--source',type=Path)
    a=p.parse_args();r=a.root.resolve();d=a.output.resolve()/('numeric-'+str(time.time_ns()));d.mkdir(parents=True)
    cap=a.source or r/'tests/experimental/c04-direct/capabilities.hua'
    source_text=cap.read_bytes();shutil.copy2(cap,d/'capabilities-source.hua')
    f=d/'main.hua';f.write_text(fixture(cap,r/'tests/experimental/c04-symbolic/capabilities.hua'),encoding='utf-8',newline='\n')
    shutil.copy2(r/'selfhost/type_rules.hua',d/'type_rules.hua')
    expected=None;rows=[]
    for mode in ('interpret','run','build','huab'):
        if mode=='huab':f.rename(f.with_suffix('.hidden'))
        args=[str(a.exe.resolve()),'run' if mode=='huab' else mode,str(f.with_suffix('.huab') if mode=='huab' else f)]
        if mode=='build':args+=['-o',str(f.with_suffix('.huab'))]
        start=time.monotonic();q=run(args,capture_output=True,timeout=30)
        (d/(mode+'.stdout')).write_bytes(q.stdout);(d/(mode+'.stderr')).write_bytes(q.stderr)
        assert q.returncode==0,(mode,q.stderr.decode())
        if mode!='build':
            lines=q.stdout.decode().splitlines()
            assert len(lines)==1 and lines[0].startswith('NUMERIC-PASSED '),lines
            if expected is None:expected=lines
            else:assert lines==expected
        rows.append(dict(mode=mode,seconds=time.monotonic()-start))
    assert cap.read_bytes()==source_text,'source changed during verification'
    result=dict(status='passed',assertions_per_path=int(expected[0].split()[1]),paths=3,rows=rows)
    (d/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result))
if __name__=='__main__':main()
