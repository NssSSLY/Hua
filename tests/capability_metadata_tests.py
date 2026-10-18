"""Private capability helpers: immutable snapshots, edit batches and versioned projections.
No input program, compiler own source, or generations are executed.
"""
from pathlib import Path
import argparse,json,re,shutil,sys,time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from audit_bootstrap_phases import command,save,digest
from capability_instantiation_tests import literal,view,value

def extract(text,name):
    match=re.search(r'(?ms)^fn '+name+r'\(.*?(?=^(?:pub )?fn |\Z)',text)
    assert match,name
    return match.group()

def source(cap,width):
    text=cap.read_text(encoding='utf-8')
    decl=text[text.index('pub struct View {'):text.index('# Node consumers')]
    env=text[text.index('struct Environment {'):text.index('fn view(')].replace('unit modules.Unit','unit int')
    names=('view','value','record','split_origin','depth','projected','note_prefix','note_type','prefix_index','materialized','copy_view','copied','merge_view','field_fact','merge','select_root','select','select_cached','attach','path_owner','path_tail','value_path','lookup_path','commit','store','begin_updates','end_updates','editable','put','same_storage','storage_aliases','same_flags','same_view','same_fields','same_value','snapshot','environment')
    functions='\n'.join(extract(text,n) for n in names).replace('unit modules.Unit','unit int')
    raw=value(view({'original|':False},typ='mod#R'),{'#@left':view({'left|':False},typ='mod#R'),'#@right':view({'right|':True},w=False,ro=True,typ='mod#R'),**{'#@left#@f'+str(i):view({'left|#@f'+str(i):False},typ='[]int') for i in range(width)},**{'#@right#@f'+str(i):view({'right|#@f'+str(i):True},w=False,ro=True,typ='[]int') for i in range(width)}})
    main=r'''fn main(){
var input=INPUT
let original=unwrap(json.encode(input))
var e=environment(0,"test",map[string]Value{"a":input,"b":input})
let before=snapshot(e.values)
let old=unwrap(e.values["a"])
var replacement=select(input,"#@right","mod#R")
let replacement_before=unwrap(json.encode(replacement))
# Root replacement shares immutable tables; a descendant write must detach.
put(e,"a",replacement,false,false)
let replaced=unwrap(e.values["a"])
let replaced_before=unwrap(json.encode(replaced))
put(e,"a#@f0",value("[]int",true),false,false)
print(original==unwrap(json.encode(input)),same_value(old,unwrap(before["a"])),replacement_before==unwrap(json.encode(replacement)),replaced_before==unwrap(json.encode(replaced)))
# Batch writable handles must not survive a whole-root replacement.
begin_updates(e)
put(e,"a#@f0",value("[]int",false),false,false)
put(e,"a",replacement,false,false)
put(e,"a#@f0",value("[]int",true),false,false)
put(e,"a#@f1",value("[]int",true),true,true)
end_updates(e)
print(replacement_before==unwrap(json.encode(replacement)),original==unwrap(json.encode(unwrap(e.values["b"]))))
# Attach/merge may share published inputs, but editing the destination detaches.
var dest=value("mod#Outer",true)
attach(dest,"#@nested",input,true)
let attached=unwrap(json.encode(dest))
var joined=merge(dest,dest)
joined.root.origins["fresh|"]=true
print(original==unwrap(json.encode(input)),attached==unwrap(json.encode(dest)),len(joined.root.origins)==len(dest.root.origins)+1)
# Exact suffix identities and versions distinguish sibling projections and edits.
let left=select(input,"#@left","mod#R")
let right=select(input,"#@right","mod#R")
let l=select_cached(e,left,"#@f0","[]int","1:b#@left",0)
let r=select_cached(e,right,"#@f0","[]int","1:b#@right",0)
print(same_value(l,select(left,"#@f0","[]int")),same_value(r,select(right,"#@f0","[]int")),!same_value(l,r))
let bversion=unwrap_or(e.versions["b"],0)
put(e,"b#@left#@f0",value("[]int",false),false,false)
let current=select(unwrap(e.values["b"]),"#@left","mod#R")
let changed=select_cached(e,current,"#@f0","[]int","1:b#@left",unwrap(e.versions["b"]))
let retained=select_cached(e,left,"#@f0","[]int","1:b#@left",bversion)
print(same_value(changed,select(current,"#@f0","[]int")),same_value(retained,l),!same_value(changed,l))
# Branch restores reuse old stamps, while the allocator continues monotonically.
let saved=snapshot(e.values)
let versions=clone(e.versions)
put(e,"b#@left#@f0",value("[]int",true),false,false)
let first_version=unwrap(e.versions["b"])
e.values=snapshot(saved)
e.versions=clone(versions)
put(e,"b#@left#@f0",value("[]int",true),false,false)
print(unwrap(e.versions["b"])>first_version)
# Removing a subtree leaves conservative index keys; absent fields must vanish.
put(e,"b#@left",value("mod#R",true),false,false)
let cleared=select(unwrap(e.values["b"]),"#@left","mod#R")
print(len(materialized(cleared).fields)==0,original==unwrap(json.encode(input)))
# Identical backing tables with different cursors are different logical values.
var sibling=select(input,"#@right","mod#R")
sibling.root=left.root
# An unrelated changed backing field cannot make an equal subtree unequal.
var unrelated=clone(input)
unrelated.fields["#@noise"]=view("int",true)
print(!same_value(left,sibling),same_value(left,select(unrelated,"#@left","mod#R")))
# Type changes leave stale buckets: actual fields/type must filter them.
var item=value("[]int",true)
item.root.origins["shared|"]=false
var holder=value("mod#Holder",true)
holder.fields["#@item"]=item.root
holder.prefixes=prefix_index(holder.fields)
var indexed=environment(0,"indexed",map[string]Value{"box":holder})
let old_aliases=unwrap(storage_aliases(indexed,item.root))
var type_replacement=value("[]string",true)
type_replacement.root.origins["shared|"]=false
put(indexed,"box#@item",type_replacement,false,false)
let stale=unwrap(storage_aliases(indexed,item.root))
let fresh=unwrap(storage_aliases(indexed,type_replacement.root))
var legacy=clone(unwrap(indexed.values["box"]))
delete(legacy.prefixes,"@types")
var fallback=environment(0,"legacy",map[string]Value{"box":legacy})
let legacy_aliases=unwrap(storage_aliases(fallback,type_replacement.root))
var projected=environment(0,"projected",map[string]Value{"part":left})
let selected=field_fact(left,"#@f0","[]int")
let selected_aliases=unwrap(storage_aliases(projected,selected))
print(len(old_aliases)==1 && old_aliases[0]=="box#@item",len(stale)==0,len(fresh)==1 && fresh[0]=="box#@item",len(legacy_aliases)==1 && legacy_aliases[0]=="box#@item",len(selected_aliases)==1 && selected_aliases[0]=="part#@f0")
}'''
    return 'import type_rules as rules\nimport std.list as list\nimport std.strings as strings\nimport std.json as json\n'+decl+env+functions+main.replace('INPUT',literal(raw,'Value'))

def execute(exe,root,out,cap,tag):
    dest=out/tag;dest.mkdir(parents=True,exist_ok=False)
    shutil.copyfile(root/'selfhost/type_rules.hua',dest/'type_rules.hua')
    rows=[]
    expected=['true true true true','true true','true true true','true true true','true true true','true','true true','true true','true true true true true']
    for width in (1,9,32):
        p=dest/('width'+str(width)+'.hua');p.write_text(source(cap,width),encoding='utf-8',newline='\n');artifact=p.with_suffix('.huab')
        outputs=[]
        for mode in ('interpret','run','build'):
            row,stdout,_=command(root,out,tag+'-w'+str(width)+'-'+mode,[exe,mode,p]+(['-o',artifact] if mode=='build' else []),60)
            if mode!='build':outputs.append(stdout.decode('utf-8').splitlines());assert outputs[-1]==expected,(mode,outputs[-1])
        hidden=p.with_suffix('.hidden');p.rename(hidden)
        try:
            row,stdout,_=command(root,out,tag+'-w'+str(width)+'-huab',[exe,'run',artifact],60)
            outputs.append(stdout.decode('utf-8').splitlines());assert outputs[-1]==expected,outputs[-1]
        finally:hidden.rename(p)
        rows.append(dict(width=width,source_sha256=digest(p),artifact_sha256=digest(artifact),output=expected))
    result=dict(status='passed',cases=len(rows),paths=['AST','source VM','source-free HUAB'],assertions=3*3*25,rows=rows)
    save(dest/'result.json',result);print(json.dumps({k:v for k,v in result.items() if k!='rows'}))
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('exe',type=Path);a.add_argument('root',type=Path);a.add_argument('output',type=Path);a.add_argument('--source',type=Path);a.add_argument('--tag',default='metadata-'+str(time.time_ns()));v=a.parse_args()
    execute(v.exe.resolve(),v.root.resolve(),v.output.resolve(),(v.source or v.root/'selfhost/capabilities.hua').resolve(),v.tag)
