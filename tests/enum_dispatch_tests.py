"""Large enum match dispatch: all leaves, wildcard, payloads and lexical cleanup."""
from pathlib import Path
import argparse,json,sys,time
sys.path.insert(0,str(Path(__file__).resolve().parent/'tools'))
from audit_bootstrap_phases import command,save,digest

def source():
    declarations=[];checks=[];expected=[]
    for width in (2,7,8,9,16,59):
        name='E'+str(width)
        declarations.append('enum '+name+'{'+','.join('V%02d(int)'%i for i in range(width))+'}\n')
        arms='\n'.join('V%02d(n)=>{return n+%d}'%(i,100*i) for i in range(width))
        declarations.append('fn pick'+str(width)+'(x '+name+') int {match x{'+arms+'}}\n')
        checks.append('print('+','.join('pick%d(%s.V%02d(%d))'%(width,name,i,i) for i in range(width))+')')
        expected.append(' '.join(str(i*101) for i in range(width)))
    declarations.append('var calls=0\nfn input() E59{calls+=1\nreturn E59.V58(58)}\n')
    declarations.append('fn mark(x int){print(x)}\n')
    explicit='\n'.join('V%02d(n)=>{defer mark(%d)\nreturn ok(n)}'%(i,10+i) for i in range(8))
    declarations.append('fn cleanup(x E59) Result<int>{defer mark(9)\nmatch x{'+explicit+'\n_=>{return err("stop")}}}\n')
    loop='\n'.join('V%02d(_)=>{if i==1{continue}\nif i==3{break}\nsum+=i}'%i for i in range(8))
    declarations.append('fn loop(x E59) int{var sum=0\nfor i in 0..4{match x{'+loop+'\n_=>{sum-=100}}}\nreturn sum}\n')
    declarations.append('fn main(){'+'\n'.join(checks)+'\nprint(pick59(input()),calls)\nprint(unwrap(cleanup(E59.V07(7))))\nprint(unwrap_err(cleanup(E59.V58(58))))\nprint(loop(E59.V07(7)),loop(E59.V58(58)))\nvar sum=0\nfor i in 0..2000{sum+=pick59(E59.V58(1))}\nprint(sum)\n}\n')
    expected+=['5858 1','17','9','7','9','stop','2 -400',str(5801*2000)]
    return ''.join(declarations),expected

def execute(exe,root,out,compiler=None,old=None):
    assert not out.exists();out.mkdir();(out/'commands').mkdir();p=out/'input.hua';text,expected=source();p.write_bytes(text.encode('utf-8'))
    outputs={};budgets={}
    paths=[('native-ast',exe,'interpret',p),('native-source-vm',exe,'run',p)]
    if old:paths.append(('old-source-vm',old,'run',p))
    for tag,tool,mode,entry in paths:
        _,stdout,stderr=command(root,out,tag,[tool,mode,entry,'--execution-profile','compiler','--report-budget'],90)
        lines=stdout.decode('utf-8').splitlines();assert lines==expected,(tag,lines,expected);outputs[tag]=lines;budgets[tag]=json.loads(stderr)
    artifact=out/'native.huab';command(root,out,'native-build',[exe,'build',p,'-o',artifact],90)
    if compiler:
        generated=out/'hua.huab';_,stdout,_=command(root,out,'hua-build',[exe,'run',compiler,'--execution-profile','compiler','--',p,generated],120)
        assert json.loads(stdout)['status']=='ok';command(root,out,'hua-reader',[exe,'check',generated],30)
    hidden=out/'input.hidden';p.rename(hidden)
    try:
        for tag,artifact in [('native-huab',artifact)]+([('hua-huab',generated)] if compiler else []):
            _,stdout,stderr=command(root,out,tag,[exe,'run',artifact,'--execution-profile','compiler','--report-budget'],60)
            lines=stdout.decode('utf-8').splitlines();assert lines==expected,(tag,lines,expected);outputs[tag]=lines;budgets[tag]=json.loads(stderr)
    finally:hidden.rename(p)
    if old:assert budgets['native-source-vm']['steps']<budgets['old-source-vm']['steps']*.5
    data={'status':'passed','enum_widths':[2,7,8,9,16,59],'all_leaves':101,'paths':list(outputs),'expected':expected,'budgets':budgets,'source_sha256':digest(p),'own_source_run':False}
    save(out/'result.json',data);print(json.dumps({'status':'passed','paths':list(outputs),'steps':{k:v['steps'] for k,v in budgets.items()}}))
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('exe',type=Path);a.add_argument('root',type=Path);a.add_argument('output',type=Path);a.add_argument('--compiler',type=Path);a.add_argument('--old',type=Path);a.add_argument('--unique-output',action='store_true');v=a.parse_args();dest=v.output.resolve()
    if v.unique_output:dest.mkdir(parents=True,exist_ok=True);dest=dest/('run-'+str(time.time_ns()))
    execute(v.exe.resolve(),v.root.resolve(),dest,v.compiler.resolve() if v.compiler else None,v.old.resolve() if v.old else None)
