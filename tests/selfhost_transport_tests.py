# C06 v2 transport: exact expansion, legacy compatibility, bounded decoding and publication.
from pathlib import Path
import sys,json,subprocess,importlib.util,copy,io
from unittest.mock import patch
from contextlib import redirect_stderr
hua,driver,repo,build=map(lambda x:Path(x).resolve(),sys.argv[1:])
out=build/'c06-transport';out.mkdir(exist_ok=True)
spec=importlib.util.spec_from_file_location('frontend',repo/'tools/hua_frontend.py');runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
contract=json.loads((repo/'selfhost/contract.json').read_text(encoding='utf-8'));checks=0

def call(args,code=0):
    p=subprocess.run(list(map(str,args)),capture_output=True,encoding='utf-8',timeout=600)
    assert p.returncode==code,(args,p.returncode,p.stdout[:1000],p.stderr[:2000])
    return p
source=out/'input.hua';source.write_text('fn main(){var n=0\n'+('n+=1\n'*300)+'print(n)}\n',encoding='utf-8')
raw=json.loads(call([hua,'run',repo/'selfhost/bytecode_main.hua','--execution-profile','compiler','--',source]).stdout)
assert raw['schema']=='hua.bytecode.image.v2';expanded=runner.validate_bytecode(raw,contract)
assert max(len(f['code']['blocks']) for f in raw['functions'])>=2
checks+=3
wire=out/'wire.json';legacy=out/'legacy.json'
wire.write_text(json.dumps(raw,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
legacy.write_text(json.dumps(expanded,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
for p in [wire,legacy]:
    assert call([driver,'run',p]).stdout=='300\n';checks+=1
assert call([driver,'image',wire]).stdout==call([driver,'image',legacy]).stdout;checks+=1
artifact=out/'packed.huab';call([driver,'build',wire,'-o',artifact]);assert call([hua,'run',artifact]).stdout=='300\n';checks+=1
# More than one million nodes in the legacy instruction view, under 16 MiB on wire.
big=copy.deepcopy(expanded);ops=[]
for k in range(42500):
    for tag in (9,10):ops.append({'op':tag,'site':0,'text':'','type_name':'','argument':0,'target':0,'flag':False,'constant':{'kind':0,'text':''},'names':[],'contextual':[]})
ops.append(dict(ops[0],op=0))
ops.append(dict(ops[0],op=22))
fn=big['functions'][0];fn['code']['instructions']=ops;fn['parameters']=[];fn['returns']=[]
# Existing initializer invokes main; all generated instructions have zero value stack effect.
big['instructions']=len(ops)+len(big['initializer']['instructions']);big['work']=big['instructions']
big['verification']={'instructions':big['instructions'],'reachable':big['instructions'],'slots':big['instructions'],'work':big['instructions'],'max_stack':1,'max_scopes':2,'max_iterators':0}
def packed(code):
    rows=[{'n':[o['op'],o['site'],o['argument'],o['target'],int(o['flag']),o['constant']['kind']],'s':[o['text'],o['type_name'],o['constant']['text']],'a':o['names'],'c':o['contextual']} for o in code['instructions']]
    return {'name':code['name'],'count':len(rows),'blocks':[json.dumps(rows[i:i+1024],separators=(',',':')) for i in range(0,len(rows),1024)]}
def pack(report):
    result=copy.copy(report);result['schema']='hua.bytecode.image.v2'
    sites=result.pop('sites');rows=[{'n':[s[k] for k in ['node','origin','start','end','line','column']],'s':[s['module'],s['role']]} for s in sites]
    result['site_blocks']=[json.dumps(rows[i:i+1024],separators=(',',':')) for i in range(0,len(rows),1024)]
    result['initializer']=packed(report['initializer']);result['functions']=[dict(f,code=packed(f['code'])) for f in report['functions']]
    return result
large=pack(big);largefile=out/'large.json';largefile.write_text(json.dumps(large,separators=(',',':')),encoding='utf-8')
assert len(ops)*14>1000000 and largefile.stat().st_size<16777216
assert runner.validate_bytecode(large,contract)==big
call([driver,'verify',largefile]);assert call([driver,'run',largefile]).stdout=='';checks+=4
# Malformed v2 blocks must be rejected before effects or publication.
mutations=[]
def bad(label,edit):
    report=copy.deepcopy(raw);edit(report);mutations.append((label,report))
def editrow(report,change):
    code=report['functions'][0]['code'];rows=json.loads(code['blocks'][0]);change(rows[0]);code['blocks'][0]=json.dumps(rows)
bad('invalid JSON',lambda r:r['initializer'].update(blocks=['[']))
bad('empty block',lambda r:r['initializer'].update(blocks=['[]']))
bad('record budget',lambda r:r['initializer'].update(blocks=[json.dumps([{'n':[0]*6,'s':['']*3,'a':[],'c':[]}]*1025)]))
bad('node budget',lambda r:r['initializer'].update(blocks=[json.dumps([{'n':[0]*6,'s':['']*3,'a':['x']*4096,'c':[False]*4096}]*8)]))
bad('code count',lambda r:r['functions'][0]['code'].update(count=1))
bad('missing slot',lambda r:editrow(r,lambda o:o['n'].pop()))
bad('bool integer',lambda r:editrow(r,lambda o:o['n'].__setitem__(0,True)))
bad('flag range',lambda r:editrow(r,lambda o:o['n'].__setitem__(4,2)))
bad('unknown row',lambda r:editrow(r,lambda o:o.update(extra=0)))
bad('unknown opcode',lambda r:editrow(r,lambda o:o['n'].__setitem__(0,99)))
bad('jump target',lambda r:editrow(r,lambda o:o.update(n=[6,o['n'][1],0,999999,0,0],s=['','',''])))
bad('mixed sites',lambda r:r.update(sites=[]))
bad('duplicate sites',lambda r:r['site_blocks'].append(r['site_blocks'][0]))
for label,report in mutations:
    wire.write_text(json.dumps(report),encoding='utf-8')
    try:runner.validate_bytecode(report,contract)
    except (ValueError,KeyError,TypeError):pass
    else:raise AssertionError(label+' Python accepted')
    native=call([driver,'run',wire],1 if label=='jump target' else 3)
    assert native.stdout==''
    target=out/'keep.json';target.write_text('KEEP',encoding='utf-8')
    with patch.object(runner.subprocess,'run',return_value=subprocess.CompletedProcess([],0,json.dumps(report),'')),redirect_stderr(io.StringIO()):
        assert runner.main(['emit',str(source),'--hua',str(hua),'-o',str(target)])==3
    assert target.read_text()=='KEEP';checks+=4
result={'result':'passed','checks':checks,'corruptions':len(mutations),'wire_schema':'v2; legacy v1 retained','large_instructions':big['instructions'],'large_wire_bytes':largefile.stat().st_size,'original_json_limits_unchanged':True,'artifact':'C++ container adapter; not C07'}
(out/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
