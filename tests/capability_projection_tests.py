# Immutable capability projection width controls.
from pathlib import Path
import argparse,re,json,sys,shutil
sys.path.insert(0,str(Path(__file__).resolve().parent/'tools'))
from audit_bootstrap_phases import command,save
from capability_instantiation_tests import view,value,literal
from capability_fixture_support import adapt_fixture

def make(cap,width):
    text=cap.read_text(encoding='utf-8');decl=text[text.index('pub struct View {'):text.index('# Node consumers')]
    functions=[]
    for name in ('view','value','split_origin','projected','field_fact','select_root','select','note_prefix'):
        match=re.search(r'(?ms)^fn '+name+r'\(.*?(?=^(?:pub )?fn |\Z)',text);assert match,name;functions.append(match.group())
    fields={'#@branch':view({'p|#@branch':False},typ='mod#R'),**{'#@branch#@f'+str(i):view({'p|#@branch#@f'+str(i):False},typ='[]int') for i in range(width)}}
    raw=value(view({'p|':False},typ='mod#Outer'),fields)
    main='''fn main(){var input=INPUT
let original=unwrap(json.encode(input))
var checksum=0
for i in 0..64{let branch=select(input,"#@branch","mod#R")
let leaf=select(branch,"#@f0","[]int")
checksum+=len(leaf.root.origins)}
input.root.writable=false
var branch=select(input,"#@branch","mod#R")
branch.root.writable=true
let leaf=select(branch,"#@f0","[]int")
print(checksum,leaf.root.writable,leaf.root.readonly_children)
input.root.writable=true
print(original==unwrap(json.encode(input)))
}'''.replace('INPUT',literal(raw,'Value'))
    result='import type_rules as rules\nimport std.strings as strings\nimport std.json as json\n'+decl+'\n'.join(functions)+main
    if 'readonly_fields bool' not in decl:result=result.replace(',prefix:"",readonly_fields:false','')
    return adapt_fixture(result,text)

def execute(exe,root,out,reference):
    assert not out.exists();out.mkdir();shutil.copy2(root/'selfhost/type_rules.hua',out/'type_rules.hua');rows=[]
    for version,cap in [('before',reference),('candidate',root/'selfhost/capabilities.hua')]:
        for width in (8,69,256):
            source=out/(version+'-'+str(width)+'.hua');source.write_bytes(make(cap,width).encode('utf-8'));artifact=source.with_suffix('.huab')
            command(root,out,version+'-build-'+str(width),[exe,'build',source,'-o',artifact],60)
            _,stdout,stderr=command(root,out,version+'-run-'+str(width),[exe,'run',artifact,'--execution-profile','compiler','--report-budget'],60)
            assert stdout.decode('utf-8').splitlines()==['64 false true','true']
            rows.append({'version':version,'width':width,'steps':json.loads(stderr)['steps'],'output':stdout.decode('utf-8').splitlines()})
    old=[v['steps'] for v in rows if v['version']=='before'];new=[v['steps'] for v in rows if v['version']=='candidate']
    assert new[-1]<old[-1]*.5,(old,new)
    save(out/'result.json',{'status':'passed','rows':rows,'all_outputs_equal':True,'note':'Whole program steps include literal setup; no wall speedup or full-source result inferred.'});print(json.dumps({'status':'passed','old_steps':old,'new_steps':new}))
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('exe',type=Path);parser.add_argument('root',type=Path);parser.add_argument('out',type=Path);parser.add_argument('reference',type=Path);args=parser.parse_args();execute(args.exe.resolve(),args.root.resolve(),args.out.resolve(),args.reference.resolve())
