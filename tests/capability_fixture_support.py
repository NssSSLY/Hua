"""Source-derived fixtures preserve logical facts while canonicalizing private storage."""
import re

def logical_record(value):
    if isinstance(value,list):return [logical_record(x) for x in value]
    if isinstance(value,dict):
        if 'numeric' in value and 'fields' in value:
            value=dict(value);tables=value.pop('numeric')
            fields={}
            for table in tables:
                for path,index in table['ids'].items():
                    cell=table['facts'][index];bits=cell[1]
                    fields[path]=dict(type_name=table['types'][cell[0]],writable=bool(bits&1),
                        readonly_children=bool(bits&2),deep_owned=bool(bits&4),
                        record_copy=bool(bits&8),origins=table['origins'],
                        origin_steps=table['base_steps']+table['steps'][index])
            value['fields']={**fields,**value['fields']}
        if 'copied_fields' in value and 'fields' in value:
            value=dict(value);copy=value.pop('copied_fields');before=value.pop('copy_readonly')
            if copy or before:
                # Production materialized() must be used for policy-bearing
                # exports. This guard prevents silently dropping semantics.
                raise AssertionError('export must materialize record-copy policy')
        if 'prefixes' in value:
            value=dict(value);value['prefixes']={k:v for k,v in value['prefixes'].items() if k!='@all'}
        if 'origin_steps' in value and 'origins' in value:
            terms=dict(value['origins'])
            for path in value['origin_steps'].split('|')[1:] if value['origin_steps'] else []:
                projected={}
                for origin,flag in sorted(terms.items()):
                    owner,tail=origin.split('|',1);tail+=path
                    if len(tail)>512:tail='';flag=True
                    projected[owner+'|'+tail]=flag
                terms=projected
            value={k:v for k,v in value.items() if k!='origin_steps'}
            value['origins']=terms
        if 'patches' in value and 'fields' in value:
            value=dict(value);patches=value.pop('patches');cuts=value.pop('cuts',{})
            fields={k:v for k,v in value['fields'].items() if k not in cuts}
            value['fields']={**fields,**patches}
        if 'root' in value and 'fields' in value and 'plan' in value:
            assert value['plan']==0,'export must be canonical'
            value={k:v for k,v in value.items() if k!='plan'}
        return {k:logical_record(v) for k,v in value.items()}
    return value

def storage_literals(code, additions):
    """Append private defaults at balanced record literals, preserving strings."""
    edits=[]
    for name,field in additions.items():
        for match in re.finditer(r'(?<![\]\w])'+name+r'\{',code):
            if re.search(r'\bstruct\s*$',code[:match.start()]):continue
            level=1;i=match.end();quoted=False;escaped=False;present=False
            wanted=field.split(':',1)[0]
            while i<len(code) and level:
                c=code[i]
                if quoted:
                    if escaped:escaped=False
                    elif c=='\\':escaped=True
                    elif c=='"':quoted=False
                elif c=='"':quoted=True
                elif c==':' and level==1:
                    token=re.search(r'(\w+)\s*$',code[match.end():i])
                    if token and token[1]==wanted:present=True
                elif c=='{':level+=1
                elif c=='}':level-=1
                i+=1
            assert level==0
            if not present:edits.append((i-1,(',' if code[match.end():i-1].strip() else '')+field))
    for at,value in sorted(edits,reverse=True):code=code[:at]+value+code[at:]
    return code

POOL_HELPERS = r'''struct CapExportView { type_name string
    writable bool
    readonly_children bool
    deep_owned bool
    record_copy bool
    origins map[string]bool }
struct CapExportValue { root CapExportView
    fields map[string]CapExportView
    prefixes map[string]map[string]bool
    prefix string
    readonly_fields bool
    positions []CapExportValue }
fn cap_decode_local(p opool.Pool, out mut map[string]bool, site_id int, loid int) {
    let site=unwrap(list.get(p.sites,site_id))
    if loid>=1048576 {
        out[site+"|"+unwrap(list.get(p.l_tails,loid-1048576))]=false
        return
    }
    if loid>0 {
        for laid, unused in unwrap(list.get(p.sets,loid)) {
            out[site+"|"+unwrap(list.get(p.l_tails,laid))]=false
        }
    }
}
fn cap_decode_origins(p opool.Pool, oid int) map[string]bool {
    var out=map[string]bool{}
    if oid==0 { return out }
    let poid=oid % 262144
    if poid>=131072 {
        let aid=(poid-131072) // 2
        out[opool.param_owner(p,aid)+"|"+opool.param_path(p,aid)]=(poid % 2)==1
    } else if poid>0 {
        for aid, ro in opool.param_map(p,poid) {
            out[opool.param_owner(p,aid)+"|"+opool.param_path(p,aid)]=ro
        }
    }
    let ah=oid // 262144
    if ah>0 {
        let asite=ah % 16384
        let al=ah // 16384
        if asite<16383 { cap_decode_local(p,out,asite,al) }
        else {
            let pairs=unwrap(list.get(p.lmaps,al))
            var i=0
            while i<len(pairs) {
                cap_decode_local(p,out,pairs[i],pairs[i+1])
                i+=2
            }
        }
    }
    return out
}
fn cap_add_origin(p mut opool.Pool, base_oid int, origin string, readonly bool) int {
    var owner=origin
    var tail=""
    if strings.contains(origin,"|") {
        let parts=strings.split(origin,"|")
        owner=parts[0]
        tail=parts[1]
    }
    var item=0
    if strings.contains(owner,"!") {
        let s=opool.site_id(p,owner)
        var ltail=tail
        if !strings.starts_with(ltail,"@") { ltail="@"+owner+ltail }
        item=s*262144+opool.single_local(p,ltail)*4294967296
    } else {
        item=opool.single_param(p,owner,tail,readonly)
    }
    return opool.merge_oid(p,base_oid,item)
}
fn cap_test_oid(p mut opool.Pool, origins map[string]bool) int {
    var acc=0
    for origin, readonly in origins {
        acc=cap_add_origin(p,acc,origin,readonly)
    }
    return acc
}
fn cap_export_view(p opool.Pool, v View) CapExportView {
    return CapExportView{type_name:v.type_name,writable:v.writable,readonly_children:v.readonly_children,deep_owned:v.deep_owned,record_copy:v.record_copy,origins:cap_decode_origins(p,v.oid)}
}
fn cap_empty_exports() []CapExportValue { var x []CapExportValue=[]
    return x }
fn cap_export_value(p opool.Pool, input Value) CapExportValue {
    var fields=map[string]CapExportView{}
    for k, v in input.fields { fields[k]=cap_export_view(p,v) }
    var pos=list.from_slice(cap_empty_exports())
    for item in input.positions { unwrap(list.append(pos,cap_export_value(p,item))) }
    return CapExportValue{root:cap_export_view(p,input.root),fields:fields,prefixes:input.prefixes,prefix:input.prefix,readonly_fields:input.readonly_fields,positions:list.snapshot(pos)}
}'''

def _wrap_materialized_exports(code):
    needle='json.encode(materialized('
    pos=0
    while True:
        idx=code.find(needle,pos)
        if idx<0:break
        start=idx+len('json.encode(')
        depth=1;i=idx+len(needle);quoted=False;escaped=False
        while i<len(code) and depth:
            c=code[i]
            if quoted:
                if escaped:escaped=False
                elif c=='\\':escaped=True
                elif c=='"':quoted=False
            elif c=='"':quoted=True
            elif c=='(':depth+=1
            elif c==')':depth-=1
            i+=1
        inner=code[start:i]
        rep='cap_export_value(cap_pool,'+inner+')'
        code=code[:start]+rep+code[i:]
        pos=start+len(rep)
    return code

def adapt_fixture(code,text,export_result=False):
    symbolic='origin_steps string' in text
    pooled_origins='import origin_pool as opool' in text
    if 'unit_id string' in text:
        code=re.sub(r'\benvironment\(0,','environment("0",',code)
    if 'struct FieldTable {' in text:
        code=storage_literals(code,{'Value':'numeric:[],copied_fields:false,copy_readonly:false'})
        if re.search(r'struct State\s*\{',code) and 'numeric_shapes map[string]FieldTable' not in code:
            code=re.sub(r'(struct State\s*\{)',r'\1numeric_shapes map[string]FieldTable\n',code,count=1)
            code=storage_literals(code,{'State':'numeric_shapes:map[string]FieldTable{}'})
    if symbolic:
        code=storage_literals(code,{'View':'origin_steps:""','Value':'patches:map[string]View{},cuts:map[string]bool{}'})
    if pooled_origins:
        code=code.replace(',plan:0','')
        main_at=code.index('fn main(')
        prefix_code,main_code=code[:main_at],code[main_at:]
        main_code=re.sub(r'\borigins:(map\[string\]bool\{[^{}]*\})',r'oid:cap_test_oid(cap_pool,\1)',main_code)
        main_code=re.sub(r'([\w.]+)\.origins\[("(?:\\.|[^"\\])*")\]=(true|false)',r'\1.oid=cap_add_origin(cap_pool,\1.oid,\2,\3)',main_code)
        main_code=re.sub(r'([\w.]+(?:\([^()\n]*\))?)\.origins\b',r'cap_decode_origins(cap_pool,\1.oid)',main_code)
        if export_result:
            main_code=main_code.replace('print(unwrap(json.encode(result)))','print(unwrap(json.encode(cap_export_value(cap_pool,result))))')
        main_code=_wrap_materialized_exports(main_code)
        code=prefix_code+main_code
    elif 'struct Actual {' not in text and not symbolic:
        return code.replace(',plan:0','')
    if symbolic:code=code.replace(',plan:0','')
    definitions={m[1]:m[0] for m in re.finditer(r'(?ms)^fn (\w+)\(.*?(?=^(?:pub )?fn |\Z)',text)}
    if pooled_origins:
        definitions.setdefault('instantiate','fn instantiate(p mut opool.Pool, caller string, module string, node int, source Value, actual map[string]Value) Value {\n    return instantiate_at(p,opool.site_id(p,caller+"!"+key(module,node)),source,actual,"")\n}\n')
        definitions.setdefault('select_cached','fn select_cached(p mut opool.Pool, e mut Environment, input Value, path string, type string, owner string, version int) Value {\n    return select(p,input,path,type)\n}\n')
        definitions.setdefault('same_fields','fn same_fields(a map[string]View, b map[string]View) bool { return a==b }\n')
    heads=set(re.findall(r'^fn (\w+)\(',code,re.M));needed=set(heads)
    needed.update(x for x in re.findall(r'(?<![\w.])(\w+)\(',code) if x in definitions)
    if export_result and not symbolic and not pooled_origins:needed.add('canonical_value')
    while True:
        more=set(needed)
        for name in needed:
            more.update(x for x in re.findall(r'(?<![\w.])(\w+)\(',definitions.get(name,'')) if x in definitions)
        if more==needed:break
        needed=more
    if pooled_origins:
        p_first=[n for n in needed if re.match(r'^fn '+n+r'\(p (?:mut )?opool\.Pool\b',definitions.get(n,''))]
        p_second=[n for n in needed if re.match(r'^fn '+n+r'\(s mut State,\s*p (?:mut )?opool\.Pool\b',definitions.get(n,''))]
        main_at=code.index('fn main(')
        prefix_code,main_code=code[:main_at],code[main_at:]
        if p_first:
            pat1=r'(?<![\w.])(?<!fn )('+'|'.join(sorted(p_first,key=len,reverse=True))+r')\((?!cap_pool\b)'
            main_code=re.sub(pat1,lambda m:m[1]+'(cap_pool,',main_code)
        if p_second:
            pat2=r'(?<![\w.])(?<!fn )('+'|'.join(sorted(p_second,key=len,reverse=True))+r')\((\w+),(?!\s*cap_pool\b)'
            main_code=re.sub(pat2,lambda m:m[1]+'('+m[2]+',cap_pool,',main_code)
        if 'var cap_pool=opool.create()' not in main_code:
            main_code=main_code.replace('fn main(){','fn main(){var cap_pool=opool.create()\n',1)
        code=prefix_code+main_code
    at=code.index('fn main(')
    extra_defs='\n'.join(definitions[n] for n in sorted(needed-heads) if n in definitions)
    if pooled_origins and 'struct CapExportView {' not in code:
        extra_defs=POOL_HELPERS+'\n'+extra_defs
    if extra_defs.strip():
        code=code[:at]+extra_defs+'\n'+code[at:]
    declarations={m[1]:m[0] for m in re.finditer(r'(?ms)^(?:pub )?struct (\w+) \{.*?\}',text)}
    present=set(re.findall(r'\bstruct (\w+)',code));insert=[]
    while True:
        tokens=re.sub(r'"(?:\\.|[^"\\])*"|#[^\n]*','',code+'\n'.join(insert))
        missing={n for n in declarations if n not in present and re.search(r'\b'+n+r'\b',tokens)}
        if not missing:break
        insert.extend(declarations[n] for n in sorted(missing));present.update(missing)
    if insert:
        at=code.index('fn ');code=code[:at]+'\n'.join(insert)+'\n'+code[at:]

    pooled=[n for n in needed if definitions.get(n,'').startswith('fn '+n+'(plans ')]
    if pooled:
        pattern=r'(?<![\w.])(?<!fn )('+'|'.join(sorted(pooled,key=len,reverse=True))+r')\((?!plans,)'
        code=re.sub(pattern,lambda m:m[1]+'(plans,',code)
        if 'fn main(){var plans=' not in code:code=code.replace('fn main(){','fn main(){var plans=map[int]Transform{}\n',1)
    if pooled_origins and 'import origin_pool as opool' not in code:
        code='import origin_pool as opool\n'+code
    if 'rules.' in code and 'import type_rules as rules' not in code:code='import type_rules as rules\n'+code
    if 'list.' in code and 'import std.list as list' not in code:code='import std.list as list\n'+code
    if 'strings.' in code and 'import std.strings as strings' not in code:code='import std.strings as strings\n'+code
    if export_result and not symbolic and not pooled_origins:code=re.sub(r'var result=(instantiate\([^\n]+\))',r'var result=canonical_value(plans,\1)',code,count=1)
    return code
