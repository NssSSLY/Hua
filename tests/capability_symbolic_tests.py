"""Source-derived symbolic origin, sparse overlay and transfer-version contracts."""
from pathlib import Path
import argparse,re,shutil,json,time
from process_guard import run

def fixture(text):
    structs={m[1]:m[0].replace('pub struct','struct') for m in re.finditer(r'(?ms)^(?:pub )?struct (\w+) \{.*?\}',text)}
    functions={m[1]:m[0] for m in re.finditer(r'(?ms)^fn (\w+)\(.*?(?=^(?:pub )?fn |\Z)',text)}
    needed={'view','value','projected','origin_terms','merge_view','field_fact','materialized','select','copied','instantiate_at','memo_return','memo_writes','version_summary','same_inputs','capture_inputs','same_value'}
    if 'origin_store' in functions:needed.add('origin_store')
    if 'reusable_body' in functions:needed.add('reusable_body')
    while True:
        more=needed|{x for n in needed for x in re.findall(r'(?<![\w.])(\w+)\(',functions[n]) if x in functions}
        if needed==more:break
        needed=more
    code=('import std.strings as strings\nimport std.list as list\nimport std.json as json\nimport type_rules as rules\n'+
      '\n'.join(structs[x] for x in ['View','Value','Effect','Inputs','Memo','Summary'])+r'''
struct State { summaries map[string]Summary
return_memos map[string]Memo
write_memos map[string]Memo
next_summary_version int }
'''+ '\n'.join(functions[n] for n in sorted(needed)) + r'''
fn empty_summary(input Value) Summary { return Summary{returned:input,unknown:false,saw_return:true,writes:map[string]View{},required:map[string]bool{},effects:map[string]Effect{},return_reads:map[string]bool{"p|#@f0":true},write_reads:map[string]bool{},return_version:1,write_version:1,required_version:1,control_version:1} }
fn fresh_state() State { return State{summaries:map[string]Summary{},return_memos:map[string]Memo{},write_memos:map[string]Memo{},next_summary_version:1} }
fn mark(s mut State) { var memo=unwrap(s.return_memos["site"])
memo.returned.root.type_name="hit"
s.return_memos["site"]=memo }
fn eager_step(base View, path string, type string) View {
var out=view(type,base.writable && !base.readonly_children)
out.readonly_children=base.readonly_children || !base.writable
out.deep_owned=base.deep_owned
for origin, readonly in base.origins {
let parts=split_origin(origin)
var next=parts[1]+path
var flag=readonly
if len(next)>512 {next=""
flag=true}
out.origins[parts[0]+"|"+next]=flag
}
return out
}
fn main(){
var base=view("map<string,int>",true)
base.origins["p|#@base"]=false
let original=unwrap(json.encode(base))
let projected_one=projected(base,"#@first","map<string,int>")
let nested=projected(projected_one,"#@[]","int")
print(unwrap(origin_terms(nested)["p|#@base#@first#@[]"])==false,original==unwrap(json.encode(base)),len(nested.origins)==1)
var long=view("R",true)
var suffix=""
for i in 0..510 { suffix+="a" }
long.origins["p|"+suffix]=false
let reset=projected(projected(long,"#@x","R"),"#@y","R")
print(unwrap_or(origin_terms(reset)["p|#@y"],false),!has(origin_terms(reset),"p|"+suffix+"#@x#@y"))
var repeated=base
for i in 0..400 { repeated=projected(repeated,"#@x","R") }
print(len(repeated.origin_steps)<=1024,len(origin_terms(repeated))==1)
var dense=view("R",true)
dense.origins["p|"]=false
dense.origins["p|"+suffix]=false
dense.origins["p|"+suffix+"xyz"]=true
dense.origins["q|#@q"]=true
var symbolic=dense
var equal=true
for i in 0..32 {
var path="#@x"
if i%7==0 {path=""}
symbolic=projected(symbolic,path,"R")
dense=eager_step(dense,path,"R")
if !same_flags(origin_terms(symbolic),dense.origins){equal=false}
}
print(equal)
let joined=merge_view(nested,projected_one)
print(len(origin_terms(joined))==2,original==unwrap(json.encode(base)))
var wide=value("R",true)
wide.root.origins["borrow|"]=false
for i in 0..24 { var item=view("int",true)
item.origins["borrow|#@f"+str(i)]=false
wide.fields["#@f"+str(i)]=item }
wide.prefixes=prefix_index(wide.fields)
let before=unwrap(json.encode(wide))
var changed=wide
var replacement=view("int",false)
replacement.origins["readonly|"]=true
changed.patches=map[string]View{"#@f0":replacement}
let resolved=materialized(changed)
print(len(changed.patches)==1,len(changed.fields)==24,!field_fact(changed,"#@f0","int").writable,!unwrap(resolved.fields["#@f0"]).writable,before==unwrap(json.encode(wide)))
print(same_value(changed,resolved),field_fact(changed,"#@f1","int").writable)
var tree=value("R",true)
tree.fields["#@a"]=base
tree.fields["#@a#@old"]=base
tree.fields["#@a#@old#@leaf"]=base
tree.fields["#@b"]=base
tree.prefixes=prefix_index(tree.fields)
var replaced=tree
replaced.cuts=map[string]bool{"#@a":true,"#@a#@old":true,"#@a#@old#@leaf":true,":#@a#@new":true}
replaced.patches=map[string]View{"#@a":replacement,"#@a#@new":base,"#@a#@new#@leaf":base}
let rebuilt=materialized(replaced)
print(!has(rebuilt.fields,"#@a#@old"),!has(rebuilt.fields,"#@a#@old#@leaf"),has(rebuilt.fields,"#@a#@new"),has(rebuilt.fields,"#@b"))
let subtree=materialized(select(replaced,"#@a","R"))
print(!has(subtree.fields,"#@old"),has(subtree.fields,"#@new"),has(subtree.fields,"#@new#@leaf"),len(materialized(tree).fields)==4)
var source=value("R",true)
for i in 0..12 { var item=view("int",true)
item.origins["p|#@f0"]=false
source.fields["#@g"+str(i)]=item }
source.prefixes=prefix_index(source.fields)
var summary=empty_summary(source)
var s=fresh_state()
let actual=map[string]Value{"p":wide}
let initial=memo_return(s,"site",summary,actual)
mark(s)
print(memo_return(s,"site",summary,actual).root.type_name=="hit")
var unrelated=wide
unrelated.patches=map[string]View{"#@f1":replacement}
print(memo_return(s,"site",summary,map[string]Value{"p":unrelated}).root.type_name=="hit")
let relevant=memo_return(s,"site",summary,map[string]Value{"p":changed})
print(relevant.root.type_name!="hit",!unwrap(relevant.fields["#@g0"]).writable)
mark(s)
summary.return_version=2
print(memo_return(s,"site",summary,map[string]Value{"p":changed}).root.type_name!="hit")
var versions=fresh_state()
let one=version_summary(versions,"callee",summary)
versions.summaries["callee"]=one
var effects_only=summary
effects_only.writes=clone(summary.writes)
effects_only.writes["p|#@f0"]=replacement
let two=version_summary(versions,"callee",effects_only)
print(one.return_version==two.return_version,one.write_version!=two.write_version,one.required_version==two.required_version,one.control_version==two.control_version)
versions.summaries["callee"]=two
var required_only=effects_only
required_only.required=map[string]bool{"p|#@f0":true}
let three=version_summary(versions,"callee",required_only)
print(two.return_version==three.return_version,two.write_version==three.write_version,two.required_version!=three.required_version)
var control=required_only
control.unknown=true
versions.summaries["callee"]=three
let four=version_summary(versions,"callee",control)
print(three.control_version!=four.control_version,three.return_version==four.return_version)
print(len(s.return_memos)==1,len(s.write_memos)==0)
var writes=empty_summary(source)
for i in 0..12 {
writes.effects["p|#@g"+str(i)]=Effect{owner:"p",path:"#@g"+str(i),fact:unwrap(source.fields["#@g0"])} }
writes.write_reads=map[string]bool{"p|#@f0":true}
let first_writes=memo_writes(s,"writes",writes,actual)
var memo=unwrap(s.write_memos["writes"])
memo.writes=clone(memo.writes)
var marked=unwrap(memo.writes["p|#@g0"])
marked.type_name="hit"
memo.writes["p|#@g0"]=marked
s.write_memos["writes"]=memo
print(unwrap(memo_writes(s,"writes",writes,map[string]Value{"p":unrelated})["p|#@g0"]).type_name=="hit")
writes.return_version+=1
print(unwrap(memo_writes(s,"writes",writes,actual)["p|#@g0"]).type_name=="hit")
let replaced_writes=memo_writes(s,"writes",writes,map[string]Value{"p":changed})
print(unwrap(replaced_writes["p|#@g0"]).type_name!="hit",!unwrap(replaced_writes["p|#@g0"]).writable)
writes.write_version+=1
print(unwrap(memo_writes(s,"writes",writes,actual)["p|#@g0"]).type_name!="hit")
}
''')

    if 'reusable_body' in functions:
        code=code.replace('struct State {','\n'.join(structs[n] for n in ['Consultation','BodyMemo'])+'\nstruct Function {result string}\nstruct State { bodies map[string]BodyMemo\nfunctions map[string]Function\nwork int\n',1)
        code=code.replace('return State{','return State{bodies:map[string]BodyMemo{},functions:map[string]Function{},work:0,',1)
        extra=r"""
var reuse=fresh_state()
reuse.work=10
var old=empty_summary(source)
reuse.summaries["callee"]=old
reuse.functions["callee"]=Function{result:"R"}
let readonly_actual=map[string]Value{"p":changed}
var old_result=instantiate_at("body-site",source,readonly_actual)
old_result.root.type_name="R"
reuse.bodies["caller"]=BodyMemo{work:7,calls:[Consultation{target:"callee",site:"body-site",actual:readonly_actual,returned:old_result,return_version:old.return_version,write_version:old.write_version,required_version:old.required_version,control_version:old.control_version}]}
var updated=clone(old)
var field=unwrap(updated.returned.fields["#@g0"])
field.writable=false
updated.returned.fields["#@g0"]=field
updated.return_version+=1
reuse.summaries["callee"]=updated
print(reusable_body(reuse,"caller"),reuse.work==17)
var requirement=updated
requirement.required_version+=1
reuse.summaries["callee"]=requirement
print(!reusable_body(reuse,"caller"),reuse.work==17)
reuse.summaries["callee"]=updated
reuse.work=1048570
print(!reusable_body(reuse,"caller"),reuse.work==1048570)
reuse.work=1048569
print(reusable_body(reuse,"caller"),reuse.work==1048576)
reuse.work=0
var relevant_summary=clone(updated)
var changed_field=unwrap(relevant_summary.returned.fields["#@g0"])
changed_field.type_name="string"
relevant_summary.returned.fields["#@g0"]=changed_field
relevant_summary.return_version+=1
reuse.summaries["callee"]=relevant_summary
print(!reusable_body(reuse,"caller"),reuse.work==0)
"""
        end=code.rfind('\n}')
        code=code[:end]+extra+code[end:]
    if 'struct OriginStore {' in text:
        code=code.replace('struct View {','\n'.join(structs[n] for n in ['OriginNode','OriginStore'])+'\nstruct View {',1)
        code=code.replace('struct State { summaries','struct State { origins OriginStore\nsummaries')
        code=code.replace('fn fresh_state() State { return State{','fn fresh_state(origins mut OriginStore) State { return State{origins:origins,')
        code=code.replace('fresh_state()','fresh_state(origins)')
        code=code.replace('fn main(){','fn main(){var origins=origin_store()\n',1)
        code=code.replace('base.origins["p|#@base"]=false','base.origin=origin_atom(origins,"p|#@base",false)')
        code=code.replace('long.origins["p|"+suffix]=false','long.origin=origin_atom(origins,"p|"+suffix,false)')
        code=code.replace('wide.root.origins["borrow|"]=false','wide.root.origin=origin_atom(origins,"borrow|",false)')
        code=code.replace('item.origins["borrow|#@f"+str(i)]=false','item.origin=origin_atom(origins,"borrow|#@f"+str(i),false)')
        code=code.replace('replacement.origins["readonly|"]=true','replacement.origin=origin_atom(origins,"readonly|",true)')
        code=code.replace('item.origins["p|#@f0"]=false','item.origin=origin_atom(origins,"p|#@f0",false)')
        code=code.replace('len(nested.origins)','len(origin_terms(origins,nested))').replace('len(repeated.origin_steps)<=1024','len(origins.nodes)<=1024')
        arena=[n for n,b in functions.items() if b.startswith('fn '+n+'(origins mut OriginStore,')]
        start=code.index('fn main(){');head,main=code[:start],code[start:]
        for name in arena:
            main=re.sub(r'(?<![\w.])'+name+r'\((?!origins,)',name+'(origins,',main)
        # The roots now include the iterative arena helpers through call closure.
        code=head+main
    if 'struct FieldTable {' in text:
        from capability_fixture_support import adapt_fixture
        code=adapt_fixture(code,text)
    return code

def main():
 p=argparse.ArgumentParser();p.add_argument('exe',type=Path);p.add_argument('root',type=Path);p.add_argument('output',type=Path);p.add_argument('--source',type=Path);a=p.parse_args();r=a.root.resolve();d=a.output.resolve()/('symbolic-'+str(time.time_ns()));d.mkdir(parents=True)
 source=a.source or r/'tests/experimental/c04-symbolic/capabilities.hua'
 (d/'main.hua').write_text(fixture(source.read_text(encoding='utf-8')),encoding='utf-8',newline='\n');shutil.copyfile(r/'selfhost/type_rules.hua',d/'type_rules.hua');rows=[];expected=None
 for mode in ['interpret','run','build','huab']:
  if mode=='huab':(d/'main.hua').rename(d/'main.hidden')
  cmd=[str(a.exe.resolve()),'run' if mode=='huab' else mode,str(d/('main.huab' if mode=='huab' else 'main.hua'))]
  if mode=='build':cmd+=['-o',str(d/'main.huab')]
  result=run(cmd,capture_output=True,timeout=30);(d/(mode+'.stdout')).write_bytes(result.stdout);(d/(mode+'.stderr')).write_bytes(result.stderr)
  assert result.returncode==0,(mode,result.stderr.decode('utf-8'))
  if mode!='build':
   lines=result.stdout.decode().splitlines();assert len(lines)==(26 if 'reusable_body' in source.read_text(encoding='utf-8') else 21),(mode,lines)
   assert all(set(line.split())=={'true'} for line in lines),(mode,lines)
   if expected is None:expected=lines
   else:assert lines==expected
  rows.append({'mode':mode,'status':'passed'})
 result={'status':'passed','assertions_per_path':sum(len(x.split()) for x in expected),'paths':3,'rows':rows};(d/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
if __name__=='__main__':main()
