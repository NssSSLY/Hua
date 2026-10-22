"""Source-derived capability instantiation oracle; no compiler own-source execution.
Exercises private helpers in an isolated module, without publishing a test API.
"""
from pathlib import Path
import argparse,json,re,sys,time,shutil
sys.path.insert(0,str(Path(__file__).resolve().parent/'tools'))
from audit_bootstrap_phases import command,save,digest,reduce_events
from capability_fixture_support import adapt_fixture,logical_record

def view(origins,w=True,ro=False,deep=False,copy=False,typ='R'):
    return dict(type_name=typ,writable=w,readonly_children=ro,deep_owned=deep,record_copy=copy,origins=origins)
def value(root,fields=None,positions=None):
    fs=fields or {}
    prefixes={"@types":{}}
    for path in fs:
        parts=path.split('#@')
        for i in range(2,len(parts)):prefixes.setdefault('#@'.join(parts[:i]),{})[path]=True
    for path, fact in fs.items():prefixes.setdefault("@type:"+fact["type_name"],{})[path]=True
    return dict(root=root,fields=fs,prefixes=prefixes,prefix="",readonly_fields=False,positions=positions or [])
def literal(obj,kind):
    if kind=='View':
        return 'View{'+','.join(k+':'+(json.dumps(v,ensure_ascii=True) if k=='type_name' else literal(v,'flags') if k=='origins' else str(v).lower()) for k,v in obj.items())+'}'
    if kind=='Value':
        return 'Value{root:'+literal(obj['root'],'View')+',fields:'+literal(obj['fields'],'fields')+',prefixes:'+literal(obj['prefixes'],'prefixes')+',prefix:'+json.dumps(obj.get('prefix',''))+',readonly_fields:'+str(obj.get('readonly_fields',False)).lower()+',positions:['+','.join(literal(v,'Value') for v in obj['positions'])+'],plan:0}'
    if kind=='prefixes':return 'map[string]map[string]bool{'+','.join(json.dumps(k)+':'+literal(v,'flags') for k,v in obj.items())+'}'
    ty={'flags':'bool','fields':'View','actual':'Value'}[kind]
    return 'map[string]'+ty+'{'+','.join(json.dumps(k)+':'+(str(v).lower() if kind=='flags' else literal(v,'View' if kind=='fields' else 'Value')) for k,v in obj.items())+'}'
def fixture(width,nested):
    leaf=view({'p|#@leaf':False,'other!u|#@tail':False})
    raw=value(view({'p|':False,'global|g':True}),{f'#@f{i}':leaf for i in range(width)})
    expected=value(view({'borrow|':False,'global|g':True}),{f'#@f{i}':view({'borrow|#@x':False,'caller!module:9|@other!u#@tail':False},w=False,ro=True) for i in range(width)})
    if nested:
        raw['fields']['#@f0#@child']=leaf
        expected['fields']['#@f0#@child']=expected['fields']['#@f0']
        raw['prefixes']['#@f0']={'#@f0#@child':True}
        expected['prefixes']['#@f0']={'#@f0#@child':True}
        child=value(view({'p|#@missing':False},typ='int'))
        child_expected=value(view({'borrow|#@missing':False},w=False,ro=True,typ='int'))
        record=value(view({'p|#@leaf':True},copy=True))
        record_expected=value(view({'borrow|#@x':True},copy=True))
        fresh=value(view({'local!x|@original#@leaf':True}))
        fresh_expected=value(view({'caller!module:9|@original#@leaf':True}))
        raw['positions']=[child,record,value(view({}),positions=[fresh])]
        expected['positions']=[child_expected,record_expected,value(view({}),positions=[fresh_expected])]
    actual={'p':value(view({'borrow|':False}),{'#@leaf':view({'borrow|#@x':False},w=False,ro=True)})}
    raw=value(raw["root"],raw["fields"],raw["positions"])
    expected=value(expected["root"],expected["fields"],expected["positions"])
    return raw,actual,expected

def source(cap,width,nested):
    text=Path(cap).read_text(encoding='utf-8');decl=text[text.index('pub struct View {'):text.index('# Node consumers')]
    functions=[]
    for name in ('view','value','key','record','split_origin','projected','copy_view','note_prefix','note_type','prefix_index','materialized','field_fact','instantiate_view_at','instantiate_at','instantiate_view','instantiate'):
        match=re.search(r'(?ms)^fn '+name+r'\(.*?(?=^(?:pub )?fn |\Z)',text)
        if not match and name in ('instantiate_view_at','instantiate_at'):continue
        assert match,name
        functions.append(match.group())
    raw,actual,expected=fixture(width,nested)
    main='fn main(){\nvar input=INPUT\nvar actual=ACTUAL\nlet original=unwrap(json.encode(input))\nlet actual_before=unwrap(json.encode(actual))\nvar result=instantiate("caller","module",9,input,actual)\nprint(unwrap(json.encode(result)))\nresult.root.origins["changed"]=true\nresult.prefixes["extra"]=map[string]bool{"leaf":true}\nif has(result.prefixes,"#@f0"){var descendants=unwrap(result.prefixes["#@f0"])\ndescendants["changed"]=true}\nif len(result.fields)>0 { var first=unwrap(result.fields["#@f0"])\nfirst.origins["changed"]=true }\nif len(result.positions)>0 { var first=result.positions[0]\nfirst.root.origins["changed"]=true }\nprint(original==unwrap(json.encode(input)),actual_before==unwrap(json.encode(actual)))\nprint("@@hua.phase:begin:instantiate")\nvar checksum=0\nfor i in 0..64 { let item=instantiate("caller","module",9,input,actual)\nchecksum+=len(item.fields)+len(item.positions) }\nprint("@@hua.phase:end:instantiate")\nprint(checksum)\n}\n'
    main=main.replace('INPUT',literal(raw,'Value')).replace('ACTUAL',literal(actual,'actual'))
    return adapt_fixture('import std.list as list\nimport std.strings as strings\nimport std.json as json\nimport type_rules as rules\n'+decl+'\n'.join(functions)+main,text,export_result=True),expected,64*(len(raw['fields'])+len(raw['positions']))

def execute(exe,root,out,cap,tag,probe=None):
    dest=out/tag;dest.mkdir(parents=True,exist_ok=False);shutil.copyfile(root/"selfhost/type_rules.hua",dest/"type_rules.hua");rows=[]
    for width,nested in [(0,False),(8,False),(32,False),(16,True)]:
        stem=f'w{width}-nested{int(nested)}';code,expected,checksum=source(cap,width,nested);p=dest/(stem+'.hua');p.write_text(code,encoding='utf-8',newline='\n');artifact=p.with_suffix('.huab')
        def validate(stdout):
            lines=[s for s in stdout.decode('utf-8').splitlines() if not s.startswith('@@hua.phase:')]
            assert len(lines)==3,lines
            assert logical_record(json.loads(lines[0]))==expected,(stem,logical_record(json.loads(lines[0])),expected)
            assert lines[1]=='true true',(stem,'source/actual isolation',lines[1])
            assert int(lines[2])==checksum
            return lines
        samples={};outputs=[]
        for mode in ('interpret','run','build'):
            row,stdout,stderr=command(root,out,tag+'-'+stem+'-'+mode,[exe,mode,p]+(['-o',artifact] if mode=='build' else []),limit=60)
            samples[mode]=row
            if mode!='build':outputs.append(validate(stdout))
        hidden=p.with_suffix('.hidden');p.rename(hidden)
        try:
            row,stdout,stderr=command(root,out,tag+'-'+stem+'-huab',[exe,'run',artifact],limit=60);outputs.append(validate(stdout));samples['huab']=row
            counts=None
            if probe:
                events=dest/(stem+'-events.jsonl');row,stdout,stderr=command(root,out,tag+'-'+stem+'-counts',[probe,'--phase-events',events,artifact,'kernel','unused'],limit=60);outputs.append(validate(stdout))
                counts=reduce_events([json.loads(s) for s in events.read_text(encoding='utf-8').splitlines()])['instantiate'];samples['counts']=row
        finally:hidden.rename(p)
        assert all(item==outputs[0] for item in outputs)
        rows.append(dict(width=width,nested=nested,expected=expected,output=outputs[0],timings=samples,counts=counts,source_sha256=digest(p),artifact_sha256=digest(artifact)))
        save(dest/'results.json',dict(status='running',rows=rows))
    save(dest/'results.json',dict(status='passed',rows=rows,semantic_paths=['AST','source VM','source-free HUAB'],checks=4*3*4))
    return rows

def main():
    a=argparse.ArgumentParser();a.add_argument('exe',type=Path);a.add_argument('root',type=Path);a.add_argument('output',type=Path);a.add_argument('--source',type=Path);a.add_argument('--tag');a.add_argument('--probe',type=Path);v=a.parse_args()
    rows=execute(v.exe.resolve(),v.root.resolve(),v.output.resolve(),(v.source or v.root/'selfhost/capabilities.hua').resolve(),v.tag or ('run-'+str(time.time_ns())),v.probe.resolve() if v.probe else None)
    print(json.dumps({'tag':v.tag,'status':'passed','cases':len(rows),'copies':[r['counts']['counters']['deep_clone_nodes'] if r['counts'] else None for r in rows]}))
if __name__=='__main__':main()
