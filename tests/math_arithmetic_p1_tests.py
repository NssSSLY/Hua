# HUA-D038 P1 arithmetic/hyperbolic APIs: independent bigint and Python math oracles.
from pathlib import Path
import json,math,random,struct,subprocess,sys,tempfile,zlib
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=rows=0
MIN=-(1<<63);MAX=(1<<63)-1;MOD=1<<64
OPS=['add','sub','mul','neg','abs','pow'];MODES=['checked','wrapping','saturating'];HYPER=['sinh','cosh','tanh','asinh','acosh','atanh']
APIS=[m+'_'+op for m in MODES for op in OPS]+HYPER

def integer(mode,op,a,b=0):
    if op=='pow':
        if b<0:return 'MATH_DOMAIN'
        if mode=='wrapping':v=pow(a,b,MOD)
        elif a in [0,1,-1]:v=1 if b==0 else (0 if a==0 else (-1 if a<0 and b%2 else 1))
        elif b>=64:v=MIN-1 if a<0 and b%2 else MAX+1
        else:v=pow(a,b)
    else:v={'add':lambda:a+b,'sub':lambda:a-b,'mul':lambda:a*b,'neg':lambda:-a,'abs':lambda:abs(a)}[op]()
    if mode=='checked':return 'MATH_OVERFLOW' if not MIN<=v<=MAX else str(v)
    if mode=='saturating':return str(min(MAX,max(MIN,v)))
    u=v%MOD;return str(u-MOD if u>=1<<63 else u)

with tempfile.TemporaryDirectory(prefix='hua-math-arithmetic-',dir=build) as temp:
    base=Path(temp);case=0
    def run(args,code=0):
        global checks
        p=subprocess.run([str(exe),*map(str,args)],cwd=base,capture_output=True,timeout=60)
        out=p.stdout.decode('utf-8').replace('\r\n','\n');err=p.stderr.decode('utf-8').replace('\r\n','\n')
        assert p.returncode==code,(args,p.returncode,out[:800],err[:2000]);checks+=1;return out,err
    def three(source,want=None,error=None,verify=None):
        global case,rows
        case+=1;p=base/f'case{case}.hua';p.write_text(source,encoding='utf-8',newline='\n');artifact=p.with_suffix('.huab')
        run(['check',p]);run(['build',p,'-o',artifact])
        for action,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.rename(p.with_suffix('.hidden'));run(['check',artifact])
            out,err=run([action,target],1 if error else 0)
            if error:assert error in err,(error,err)
            elif verify:verify(out)
            else:assert out==want,(out[:1200],want[:1200])
            rows+=len(out.splitlines())
        return artifact
    imports='import std.math as math\nimport std.strings as strings\n'
    show='fn show(v Result<int,string>) string { if is_err(v) { return strings.split(unwrap_err(v),":")[0] }\nreturn str(unwrap(v)) }\n'
    rng=random.Random(221104)
    values=list(dict.fromkeys([MIN,MIN+1,-MAX,-3037000500,-3,-2,-1,0,1,2,3,3037000499,3037000500,MAX-1,MAX,*[rng.randrange(MIN,MAX+1) for _ in range(12)]]))
    expected=[];statements=[]
    for op in ['neg','abs']:
        calls=[('show(math.' if m=='checked' else 'math.')+m+'_'+op+'(a)'+(')' if m=='checked' else '') for m in MODES]
        statements.append('for a in xs { print('+','.join(calls)+') }')
        for a in values:expected.append(' '.join(integer(m,op,a) for m in MODES))
    for op in ['add','sub','mul']:
        calls=[('show(math.' if m=='checked' else 'math.')+m+'_'+op+'(a,b)'+(')' if m=='checked' else '') for m in MODES]
        statements.append('for a in xs { for b in xs { print('+','.join(calls)+') } }')
        for a in values:
            for b in values:expected.append(' '.join(integer(m,op,a,b) for m in MODES))
    source=imports+show+'fn main(){ let xs=['+','.join(map(str,values))+']\n'+'\n'.join(statements)+'\n}\n'
    artifact=three(source,'\n'.join(expected)+'\n')
    data=artifact.read_bytes();assert struct.unpack_from('<I',data,8)[0]==22
    assert struct.unpack_from('<Q',data,16)[0]==len(data)-32 and struct.unpack_from('<I',data,24)[0]==zlib.crc32(data[32:])
    exp=[0,1,2,3,7,31,32,62,63,64,65,MAX]
    bases=[MIN,MIN+1,-MAX,-3,-2,-1,0,1,2,3,3037000499,3037000500,MAX]
    lines=[f'{integer("checked","pow",a,b)} {integer("wrapping","pow",a,b)} {integer("saturating","pow",a,b)}' for a in bases for b in exp]
    three(imports+show+'fn main(){for a in ['+','.join(map(str,bases))+'] { for b in ['+','.join(map(str,exp))+'] { print(show(math.checked_pow(a,b)),math.wrapping_pow(a,b),math.saturating_pow(a,b)) } } }\n','\n'.join(lines)+'\n')
    three(imports+show+'fn main(){print(show(math.checked_pow(2,-1)),show(math.checked_pow(0,-9223372036854775808)))}\n','MATH_DOMAIN MATH_DOMAIN\n')
    for mode in ['wrapping','saturating']:
        for e in [-1,MIN]:three(imports+f'fn main(){{print(math.{mode}_pow(2,{e}))}}\n',error='E4003')
    # Finite and extreme inputs. Compare tolerantly, never require libm bit identity.
    xs=[-sys.float_info.max,-711.0,-710.0,-20.0,-1.0001,-1.0,-0.9999,-0.5,-1e-300,-0.0,0.0,1e-300,0.5,0.9999,1.0,1.0001,20.0,710.0,711.0,sys.float_info.max]
    hyper_expected=[];lines=[]
    for name in HYPER:
        for x in xs:
            lines.append(f'emit(math.{name}({repr(x)}))')
            try:want=getattr(math,name)(x);assert math.isfinite(want)
            except ValueError:want='MATH_DOMAIN'
            except OverflowError:want='MATH_OVERFLOW'
            hyper_expected.append(want)
    def verify_hyper(out):
        actual=out.splitlines();assert len(actual)==len(hyper_expected)
        for got,want in zip(actual,hyper_expected):
            if isinstance(want,str):assert got==want,(got,want)
            else:
                val=float(got);assert math.isfinite(val) and math.isclose(val,want,rel_tol=2e-12,abs_tol=5e-324),(got,want)
                if want==0.0:assert math.copysign(1,val)==math.copysign(1,want),(got,want)
    three(imports+'fn emit(v Result<float,string>){if is_err(v) { print(strings.split(unwrap_err(v),":")[0]) }\nelse {print(unwrap(v))}}\nfn main(){\n'+'\n'.join(lines)+'\n}\n',verify=verify_hyper)
    # Each symbol separately requires22; wrong argument type/arity must be static errors.
    for name in APIS:
        unary=name in HYPER or name.endswith(('_neg','_abs'))
        args='1.0' if name in HYPER else ('1' if unary else '1,1')
        p=base/'symbol.hua';p.write_text(imports+f'fn main(){{ print(math.{name}({args})) }}\n',encoding='utf-8');a=base/'symbol.huab';run(['build',p,'-o',a]);assert struct.unpack_from('<I',a.read_bytes(),8)[0]==22
        wrong='1' if name in HYPER else ('i32(1)' if unary else '1.0,1')
        for expr in [f'math.{name}({wrong})',f'math.{name}()']:
            p.write_text(imports+'fn main(){ print('+expr+') }\n',encoding='utf-8');_,err=run(['check',p],1);assert 'E3004' in err or 'E3005' in err,err
    changed=bytearray(artifact.read_bytes());struct.pack_into('<I',changed,8,21);bad=base/'downgraded.huab';bad.write_bytes(changed);_,err=run(['check',bad],1);assert 'E6002' in err
    old=root/'tests/artifacts/stdlib-p1-arithmetic-20261104/baseline-runtime/hua.exe'
    if old.exists():
        p=subprocess.run([str(old),'check',str(artifact)],capture_output=True,timeout=60);assert p.returncode==1 and b'E6002' in p.stderr;checks+=1
    a=three(imports+'fn main(){print(math.popcount(-1),math.rotate_left(1,1))}\n','64 2\n');assert struct.unpack_from('<I',a.read_bytes(),8)[0]==21
report={'apis':APIS,'process_checks':checks,'oracle_rows':rows,'integer_values':len(values),'integer_pairs':len(values)**2,'pow_cases':len(bases)*len(exp),'hyperbolic_cases':len(hyper_expected),'paths':['AST','source VM','source-free HUAB'],'format':22}
(build/'math_arithmetic_p1_verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report))