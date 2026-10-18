"""C06 verifier metadata: independent expectations and frozen-reference equivalence."""
from pathlib import Path
import argparse,ast,json,shutil,sys,time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from audit_bootstrap_phases import command,save

def hx(x):
    if isinstance(x,bool):return str(x).lower()
    if isinstance(x,str):return json.dumps(x,ensure_ascii=False)
    if isinstance(x,list):return '['+','.join(hx(v) for v in x)+']'
    return str(x)
def op(k,text='',typ='',target=0,arg=0,names=None,flags=None):
    return 'op('+','.join(hx(x) for x in [k,target,arg,text,typ,names or [],flags or []])+')'
def param(name='p',typ='int',site=1):
    return 'b.Parameter{name:'+hx(name)+',type_name:'+hx(typ)+',writable:false,site:'+str(site)+'}'
def fn(name='f',params=None,ops=None):
    return 'function('+hx(name)+',['+','.join(params or [])+'],['+','.join(ops or [op(0),op(22)])+'])'
def image(funcs=None,init=None):
    return 'image(['+','.join(funcs or [])+'],['+','.join(init or [op(0),op(22)])+'])'
def stats(instructions,reachable=None,slots=None,max_stack=1):
    return dict(instructions=instructions,reachable=reachable if reachable is not None else instructions,slots=slots if slots is not None else instructions//2*3,work=reachable if reachable is not None else instructions,max_stack=max_stack,max_scopes=0,max_iterators=0)
def fixtures(root):
    cases=[]
    def add(name,expression,expected=None,error=None,edit='',position=None):
        cases.append(dict(name=name,expression=expression,expected=expected,error=error,edit=edit,position=position))
    add('empty_functions',image(),stats(2))
    add('own_parameter',image([fn(params=[param()],ops=[op(1,'p'),op(22)])]),stats(4))
    add('parameter_location',image([fn(params=[param()],ops=[op(12,'p'),op(0),op(15,'='),op(22)])]),stats(6,slots=11,max_stack=2))
    add('parameter_unicode',image([fn(params=[param('参数')],ops=[op(1,'参数'),op(22)])]),stats(4))
    add('parameter_max_name',image([fn(params=[param('p'*4096)],ops=[op(1,'p'*4096),op(22)])]),stats(4))
    add('initializer_name_collision',image([fn('<initialize>',[param()],[op(1,'p'),op(22)])],[op(1,'p'),op(22)]),stats(4))
    add('shared_parameter_names',image([fn('f',[param()],[op(1,'p'),op(22)]),fn('g',[param()],[op(1,'p'),op(22)])]),stats(6))
    add('parameter_shadows_global_function',image([fn('f',[param('g')],[op(1,'g'),op(22)]),fn('g')]),stats(6))
    bind=[op(0),op(2,'g','int',flags=[False]),op(0),op(22)]
    add('global_binding',image([fn(ops=[op(1,'g'),op(22)])],bind),stats(6,slots=9))
    add('forward_function_name',image([fn('f',ops=[op(1,'g'),op(22)]),fn('g')]),stats(6))
    add('builtin_value',image(init=[op(1,'print'),op(22)]),stats(2))
    add('discard_name',image(init=[op(1,'_'),op(22)]),stats(2))
    add('unreachable_local_declaration',image(init=[op(0),op(22),op(2,'local','int',flags=[False]),op(1,'local')]),stats(4,reachable=2,slots=3))
    add('unreachable_global_available',image([fn(ops=[op(1,'g'),op(22)])],[op(0),op(22),op(2,'g','int',flags=[False])]),stats(5,reachable=4,slots=6))
    add('multi_global_names',image([fn(ops=[op(1,'b'),op(22)])],[op(0),op(22),op(35,typ='multi<int,int>',arg=2,names=['a','b'])]),stats(5,reachable=4,slots=6))
    add('missing_parameter',image([fn('f',[param('p')],[op(1,'other'),op(22)])]),error='unknown binding/function',position='E6002@7:11 [input.hua] C06 unknown binding/function')
    add('parameters_do_not_leak',image([fn('f',[param('p')]),fn('g',ops=[op(1,'p'),op(22)])]),error='unknown binding/function')
    add('initializer_has_no_function_parameters',image([fn('f',[param('p')])],[op(1,'p'),op(22)]),error='unknown binding/function')
    add('builtin_is_not_location',image(init=[op(12,'print'),op(0),op(15,'='),op(22)]),error='unknown binding/function')
    add('unreachable_unknown_name',image(init=[op(0),op(22),op(1,'missing')]),error='unknown binding/function')
    add('unreachable_bad_type',image(init=[op(0),op(22),op(0,typ='Missing')]),error='invalid metadata')
    add('duplicate_parameter',image([fn(params=[param(),param()])]),error='invalid parameter metadata')
    add('parameter_too_long',image([fn(params=[param('p'*4097)])]),error='invalid parameter metadata')
    add('parameter_bad_site',image([fn(params=[param(site=99)])]),error='invalid parameter metadata')
    add('main_parameter',image([fn('main',[param()])]),error='main requires zero parameters')
    add('duplicate_function',image([fn(),fn()]),error='invalid function metadata')
    add('code_name_mismatch',image([fn()]),error='invalid function metadata',edit='var f=unwrap(l.get(input.functions,0))\nf.code.name="different"\nunwrap(l.set(input.functions,0,f))')
    add('function_metadata_before_initializer_flow',image([fn(params=[param(),param()])],[op(3),op(22)]),error='invalid parameter metadata',position='E6002@1:1 C06 invalid parameter metadata')
    add('initializer_error_before_function_code_error',image([fn(ops=[op(1,'missing'),op(22)])],[op(3),op(22)]),error='operand stack underflow/kind mismatch',position='E6002@7:11 [input.hua] C06 operand stack underflow/kind mismatch')
    add('metadata_before_reachable_flow',image(init=[op(3),op(22),op(41)]),error='unsupported/unknown opcode',position='E6002@7:11 [input.hua] C06 unsupported/unknown opcode')
    add('first_instruction_diagnostic',image(init=[op(1,'first'),op(41)]),error='unknown binding/function',position='E6002@7:11 [input.hua] C06 unknown binding/function')
    add('instruction_count',image(),error='instruction count mismatch',edit='input.instructions=3\ninput.work=3')
    tree=ast.parse((root/'tests/selfhost_bytecode_tests.py').read_text(encoding='utf-8'))
    bad=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='bad_cases' for t in n.targets))
    for name,ops in bad.items():
        expression='image([],['+','.join('op('+','.join(hx(v) for v in row)+')' for row in ops)+'])'
        add('existing_'+name,expression,error='E6002')
    return cases

def source(cases):
    header=r'''import bytecode as b
import bytecode_verify as v
import std.list as l
import std.json as json
fn op(k int,target int,argument int,text string,t string,names []string,flags []bool)b.Instruction{return b.Instruction{op:k,site:1,text:text,type_name:t,argument:argument,target:target,flag:false,constant:b.Scalar{kind:0,text:""},names:names,contextual:flags}}
fn function(name string,parameters []b.Parameter,ops []b.Instruction)b.Function{return b.Function{name:name,site:1,published:false,writable:false,parameters:parameters,returns:["int"],code:b.Code{name:name,instructions:l.from_slice(ops)}}}
fn image(functions []b.Function,ops []b.Instruction)b.Image{var structures []b.Structure=[]
var count=len(ops)
for f in functions{count+=l.len(f.code.instructions)}
return b.Image{entry:"input.hua",sites:l.from_slice([b.Site{node:-1,module:"input.hua",origin:-1,start:0,end:0,line:1,column:1,role:"initializer"},b.Site{node:0,module:"input.hua",origin:0,start:2,end:3,line:7,column:11,role:"source"}]),structures:l.from_slice(structures),functions:l.from_slice(functions),initializer:b.Code{name:"<initialize>",instructions:l.from_slice(ops)},instructions:count,work:count}}
struct Row{name string
accepted bool
error string
verification v.Verification}
fn test(name string,input b.Image){let result=v.verify(input)
var text=""
var checked=v.Verification{instructions:0,reachable:0,slots:0,work:0,max_stack:0,max_scopes:0,max_iterators:0}
if is_err(result){text=unwrap_err(result)}else{checked=unwrap(result)}
print(unwrap(json.encode(Row{name:name,accepted:is_ok(result),error:text,verification:checked})))}
fn main(){
'''
    lines=[]
    for i,c in enumerate(cases):
        name='case'+str(i);lines+=['var '+name+'='+c['expression'],c['edit'].replace('input',name),'test('+hx(c['name'])+','+name+')']
    return header+'\n'.join(lines)+'\n}\n'

def execute(exe,root,out,tag,reference=None,probe=None):
    dest=out/tag;dest.mkdir(parents=True,exist_ok=False);cases=fixtures(root);code=source(cases)
    results={};checks=0
    for version,origin in [('candidate',root/'selfhost')]+([('reference',reference)] if reference else []):
        src=dest/version;src.mkdir()
        for p in origin.glob('*.hua'):shutil.copy2(p,src/p.name)
        driver=src/'metadata_main.hua';driver.write_text(code,encoding='utf-8',newline='\n');artifact=dest/(version+'.huab')
        command(root,out,tag+'-'+version+'-build',[exe,'build',driver,'-o',artifact],90)
        modes=['interpret','run','huab']+(['sb1'] if probe else []) if version=='candidate' else ['huab']
        for mode in modes:
            tool=probe if mode=='sb1' else exe;target=artifact if mode=='huab' else driver
            _,stdout,_=command(root,out,tag+'-'+version+'-'+mode,[tool,'run' if mode in ['huab','sb1'] else mode,target,'--execution-profile','compiler','--report-budget'],90)
            rows=[json.loads(s) for s in stdout.decode('utf-8').splitlines()];assert len(rows)==len(cases)
            for c,r in zip(cases,rows):
                assert r['name']==c['name']
                assert r['accepted']==(c['error'] is None),(c['name'],r)
                if c['error']:
                    assert c['error'] in r['error'] and r['error'].startswith('E6002@'),(c['name'],r)
                    if c['position']:assert r['error']==c['position'],(c['name'],r)
                else:assert r['verification']==c['expected'],(c['name'],r,c['expected'])
                checks+=3
            results[version+'-'+mode]=rows
        if version=='candidate':
            hidden=dest/'hidden-candidate';src.rename(hidden)
            try:
                _,stdout,_=command(root,out,tag+'-source-free',[exe,'run',artifact,'--execution-profile','compiler'],90)
                assert [json.loads(s) for s in stdout.decode('utf-8').splitlines()]==results['candidate-huab']
            finally:hidden.rename(src)
    assert all(r==results['candidate-huab'] for r in results.values()),'complete verifier result changed'
    result=dict(status='passed',cases=len(cases),checks=checks,modes=list(results),full_old_new_equivalence=bool(reference),source_free=True,rows=results['candidate-huab']);save(dest/'result.json',result)
    print(json.dumps({k:v for k,v in result.items() if k!='rows'}))
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('exe',type=Path);a.add_argument('root',type=Path);a.add_argument('output',type=Path);a.add_argument('--tag',default='verifier-'+str(time.time_ns()));a.add_argument('--reference',type=Path);a.add_argument('--probe',type=Path);v=a.parse_args()
    execute(v.exe.resolve(),v.root.resolve(),v.output.resolve(),v.tag,v.reference.resolve() if v.reference else None,v.probe.resolve() if v.probe else None)
