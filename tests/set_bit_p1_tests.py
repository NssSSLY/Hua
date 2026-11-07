# HUA-D038 P1 Set/bit tools: independent 64-bit oracle, three execution paths.
from pathlib import Path
import json,random,struct,subprocess,sys,tempfile,zlib
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0;assertions=0
MASK=(1<<64)-1

def signed(n):
    n &= MASK
    return n-(1<<64) if n>>63 else n

def rotate(x,n,left):
    bits=x&MASK
    if not n:return x
    return signed((bits<<n)|(bits>>(64-n))) if left else signed((bits>>n)|(bits<<(64-n)))

with tempfile.TemporaryDirectory(prefix='hua-set-bit-',dir=build) as temp:
    base=Path(temp);case=0
    def run(args,code=0):
        global checks
        p=subprocess.run([str(exe),*map(str,args)],cwd=base,capture_output=True,timeout=60)
        out=p.stdout.decode('utf-8').replace('\r\n','\n');err=p.stderr.decode('utf-8').replace('\r\n','\n')
        assert p.returncode==code,(args,p.returncode,out[:1000],err[:2000]);checks+=1;return out,err
    def three(text,want=None,error=None):
        global case,assertions
        case+=1;p=base/f'case{case}.hua';p.write_text(text,encoding='utf-8',newline='\n');artifact=p.with_suffix('.huab')
        run(['check',p]);run(['build',p,'-o',artifact])
        for action,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.rename(p.with_suffix('.hidden'));run(['check',artifact])
            out,err=run([action,target],1 if error else 0)
            if error:assert error in err,(error,err)
            else:assert out==want,(out[:1500],want[:1500])
            assertions+=len(want.splitlines()) if want is not None else 1
        return artifact
    rng=random.Random(211104)
    values=list(dict.fromkeys([0,1,-1,-(1<<63),(1<<63)-1,*[signed(1<<i) for i in range(64)],*[signed(rng.getrandbits(64)) for _ in range(24)]]))
    body='import std.math as math\nfn main() {\n    for x in ['+','.join(map(str,values))+'] {\n        print(math.popcount(x), math.leading_zeros(x), math.trailing_zeros(x))\n        for n in 0..64 {\n            let a=math.rotate_left(x,n)\n            let b=math.rotate_right(x,n)\n            print(a,b,math.rotate_right(a,n)==x,math.rotate_left(b,n)==x)\n        }\n    }\n}\n'
    want=[]
    for x in values:
        bits=x&MASK
        want.append(f'{bits.bit_count()} {64-bits.bit_length()} {(bits&-bits).bit_length()-1 if bits else 64}')
        for n in range(64):want.append(f'{rotate(x,n,True)} {rotate(x,n,False)} true true')
    artifact=three(body,'\n'.join(want)+'\n')
    data=artifact.read_bytes();assert struct.unpack_from('<I',data,8)[0]==21
    assert struct.unpack_from('<Q',data,16)[0]==len(data)-32
    assert struct.unpack_from('<I',data,24)[0]==zlib.crc32(data[32:])
    old=bytearray(data);struct.pack_into('<I',old,8,20);bad=base/'downgraded.huab';bad.write_bytes(old)
    _,err=run(['check',bad],1);assert 'E6002' in err
    baseline=root/'tests/artifacts/stdlib-p1-bits-20261104/baseline-runtime/hua.exe'
    if baseline.exists():
        p=subprocess.run([str(baseline),'check',str(artifact)],capture_output=True,timeout=60)
        assert p.returncode==1 and b'E6002' in p.stderr;checks+=1
    for name in ['rotate_left','rotate_right']:
        for n in [-1,64,-(1<<63),(1<<63)-1]:
            three(f'import std.math as math\nfn main() {{ print(math.{name}(1,{n})) }}\n',error='E4003')
    # Each new symbol alone requires format 21, rather than relying on a mixed image.
    for expr in ['math.popcount(0)','math.leading_zeros(0)','math.trailing_zeros(0)','math.rotate_left(1,0)','math.rotate_right(1,0)']:
        p=base/'one.hua';p.write_text('import std.math as math\nprint('+expr+')\n',encoding='utf-8');a=base/'one.huab';run(['build',p,'-o',a]);assert struct.unpack_from('<I',a.read_bytes(),8)[0]==21
    # Boolean, integer and Unicode string keys; the oracle ignores original values.
    sources=[];expected=[]
    def lit(v):return json.dumps(v,ensure_ascii=False).lower() if isinstance(v,bool) else json.dumps(v,ensure_ascii=False)
    for typ,keys in [('bool',[False,True]),('int',[-(1<<63),-7,0,1,99,(1<<63)-1]),('string',['alpha','z','中文','🦊'])]:
        cases=[([],[]),(keys,[]),([],keys),(keys,keys),(keys[:2],keys[1:])]
        cases += [(rng.sample(keys,rng.randrange(len(keys)+1)),rng.sample(keys,rng.randrange(len(keys)+1))) for _ in range(8)]
        for a,b in cases:
            left='map['+typ+']bool{'+','.join(lit(k)+':false' for k in a)+'}'
            right='map['+typ+']string{'+','.join(lit(k)+':"value"' for k in b)+'}'
            sources.append('print(maps.keys(sets.symmetric_difference('+left+','+right+')))')
            out=sorted(set(a)^set(b));expected.append('['+', '.join(str(x).lower() if isinstance(x,bool) else str(x) for x in out)+']')
    a=three('import std.sets as sets\nimport std.maps as maps\nfn main(){\n'+'\n'.join(sources)+'\n}\n','\n'.join(expected)+'\n')
    assert struct.unpack_from('<I',a.read_bytes(),8)[0]==21
    three('''import std.sets as sets
import std.maps as maps
fn read_only(a map[int]bool,b map[int]string) map[int]bool { return sets.symmetric_difference(a,b) }
fn main(){
    var a=map[int]bool{1:false,2:true}
    var b=map[int]string{2:"x",3:"y"}
    var result=read_only(a,b)
    result[1]=false
    result[4]=true
    print(maps.keys(result),result[1],a[1],len(a),len(b),has(a,4),has(b,4))
    print(len(sets.symmetric_difference(a,a)))
    var nils=map[int]int?{1:nil,2:9}
    print(maps.keys(sets.symmetric_difference(nils,map[int]bool{2:false})))
}
''','[1, 3, 4] false false 2 2 false false\n0\n[1]\n')
    for body in ['print(math.popcount(1.0))','print(math.popcount(i32(1)))','print(math.leading_zeros(u64(1)))','print(math.trailing_zeros(true))','print(math.rotate_left(1,i32(2)))','print(math.rotate_right(f64(1.0),2))','print(math.rotate_left(1))','print(math.popcount(1,2))','print(sets.symmetric_difference(map[int]bool{},map[string]bool{}))','print(sets.symmetric_difference([1],[2]))']:
        p=base/'bad.hua';p.write_text('import std.math as math\nimport std.sets as sets\nfn main(){ '+body+' }\n',encoding='utf-8');_,err=run(['check',p],1);assert 'E3004' in err or 'E3005' in err,err
    old=three('import std.sets as sets\nfn main(){print(len(sets.union(map[int]bool{1:false},map[int]bool{2:false})))}\n','2\n')
    assert struct.unpack_from('<I',old.read_bytes(),8)[0]==10
report={'process_checks':checks,'oracle_output_rows_across_paths':assertions,'integers':len(values),'rotation_counts_per_integer':64,'set_cases':len(sources),'paths':['AST','source VM','source-free HUAB'],'format':21}
(build/'set_bit_p1_verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Set/bit P1 passed:',json.dumps(report))