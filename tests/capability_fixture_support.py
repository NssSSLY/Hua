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

def adapt_fixture(code,text,export_result=False):
    symbolic='origin_steps string' in text
    if 'struct FieldTable {' in text:
        code=storage_literals(code,{'Value':'numeric:[],copied_fields:false,copy_readonly:false'})
        if re.search(r'struct State\s*\{',code) and 'numeric_shapes map[string]FieldTable' not in code:
            code=re.sub(r'(struct State\s*\{)',r'\1numeric_shapes map[string]FieldTable\n',code,count=1)
            code=storage_literals(code,{'State':'numeric_shapes:map[string]FieldTable{}'})
    if symbolic:
        code=storage_literals(code,{'View':'origin_steps:""','Value':'patches:map[string]View{},cuts:map[string]bool{}'})
    if 'struct Actual {' not in text and not symbolic:
        return code.replace(',plan:0','')
    if symbolic:code=code.replace(',plan:0','')
    definitions={m[1]:m[0] for m in re.finditer(r'(?ms)^fn (\w+)\(.*?(?=^(?:pub )?fn |\Z)',text)}
    heads=set(re.findall(r'^fn (\w+)\(',code,re.M));needed=set(heads)
    needed.update(x for x in re.findall(r'(?<![\w.])(\w+)\(',code) if x in definitions)
    if export_result and not symbolic:needed.add('canonical_value')
    while True:
        more=set(needed)
        for name in needed:
            more.update(x for x in re.findall(r'(?<![\w.])(\w+)\(',definitions.get(name,'')) if x in definitions)
        if more==needed:break
        needed=more
    at=code.index('fn main(');code=code[:at]+'\n'.join(definitions[n] for n in sorted(needed-heads))+'\n'+code[at:]
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
    if 'rules.' in code and 'import type_rules as rules' not in code:code='import type_rules as rules\n'+code
    if 'list.' in code and 'import std.list as list' not in code:code='import std.list as list\n'+code
    if export_result and not symbolic:code=re.sub(r'var result=(instantiate\([^\n]+\))',r'var result=canonical_value(plans,\1)',code,count=1)
    return code
