# HUA-D038 step 4b (std.random, std.crypto) across three execution paths.
# Expected PRNG output comes from an independent Python model of the fixed
# algorithm (xoshiro256** seeded by SplitMix64, Lemire bounded draws); hashes
# from hashlib/hmac on the FIPS 180-4 and RFC 4231 vectors.
from pathlib import Path
import difflib,hashlib,hmac,re,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0
M=(1<<64)-1

class Model:
    def __init__(self,seed):
        x=seed&M;self.s=[]
        for _ in range(4):
            x=(x+0x9e3779b97f4a7c15)&M;z=x;z=((z^(z>>30))*0xbf58476d1ce4e5b9)&M;z=((z^(z>>27))*0x94d049bb133111eb)&M;self.s.append(z^(z>>31))
    def next(self):
        s=self.s;rot=lambda x,k:((x<<k)|(x>>(64-k)))&M;result=(rot((s[1]*5)&M,7)*9)&M;t=(s[1]<<17)&M
        s[2]^=s[0];s[3]^=s[1];s[1]^=s[2];s[0]^=s[3];s[2]^=t;s[3]=rot(s[3],45);return result
    def bounded(self,span):
        m=self.next()*span;low=m&M
        if low<span:
            threshold=((1<<64)-span)%span
            while low<threshold:m=self.next()*span;low=m&M
        return m>>64
    def int(self,lo,hi):
        v=(lo+self.bounded((hi-lo)&M))&M;return v-(1<<64) if v>=1<<63 else v
    def float53(self):return self.next()>>11
    def bool(self):return 'true' if self.next()>>63 else 'false'
    def bytes(self,n):
        out=bytearray()
        while len(out)<n:out+=self.next().to_bytes(8,'little')
        return bytes(out[:n])
    def shuffle(self,xs):
        for i in range(len(xs),1,-1):j=self.bounded(i);xs[i-1],xs[j]=xs[j],xs[i-1]
    def choice(self,xs):return xs[self.bounded(len(xs))]
    def sample(self,xs,k):
        idx=list(range(len(xs)));out=[]
        for t in range(k):j=t+self.bounded(len(idx)-t);idx[t],idx[j]=idx[j],idx[t];out.append(xs[idx[t]])
        return out

def run(command,path,*extra,cwd=None,expected=None,code=0,error=None):
    global checks
    p=subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=cwd,capture_output=True,timeout=120)
    out=p.stdout.decode('utf-8').replace('\r\n','\n');err=p.stderr.decode('utf-8')
    assert p.returncode==code,(command,path,p.returncode,out,err)
    if expected is not None:assert out==expected,(command,''.join(difflib.unified_diff(expected.splitlines(True),out.splitlines(True),'expected','actual')),err)
    if error:assert error in err,(command,out,err,error)
    checks+=1;return out
def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]
fmt=lambda xs:'['+', '.join(map(str,xs))+']'

seeds=[0,1,-1,42,(1<<63)-1,-(1<<63)]
ranges=[(0,1000),(-5,5),(-(1<<63),(1<<63)-1),(0,1),(0,3),(-(1<<62),1<<62)]
program=['import std.random as random','import std.crypto as crypto','import std.bytes as bytes','import std.encoding as encoding','import std.list as list','import std.strings as strings','fn main() {']
expected=[]
lit=lambda v:f'({v+1} - 1)' if v==-(1<<63) else str(v)
for seed in seeds:
    m=Model(seed);program.append(f'    var r{seeds.index(seed)} = random.new({lit(seed)})');name=f'r{seeds.index(seed)}'
    for lo,hi in ranges:program.append(f'    print(random.int({name}, {lit(lo)}, {lit(hi)}))');expected.append(str(m.int(lo,hi)))
    program.append(f'    print(int(random.float({name}) * 9007199254740992.0), random.bool({name}), encoding.hex_encode(random.bytes({name}, 11)))')
    expected.append(f'{m.float53()} {m.bool()} {m.bytes(11).hex()}')
    program.append(f'    var l{name} = list.from_slice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9])')
    program.append(f'    random.shuffle({name}, l{name})')
    program.append(f'    print(list.snapshot(l{name}), unwrap(random.choice({name}, [10, 11, 12, 13, 14, 15])), unwrap(random.sample({name}, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9], 4)))')
    xs=list(range(10));m.shuffle(xs);expected.append(f'{fmt(xs)} {m.choice(list(range(10,16)))} {fmt(m.sample(list(range(10)),4))}')
# clone forks the state; empty/oversized inputs are Err; bytes(0) is empty.
m=Model(5);program+=['    var a = random.new(5)','    var b = clone(a)','    print(random.int(a, 0, 1000000), random.int(b, 0, 1000000), random.int(a, 0, 7))',
    '    var e []int = []','    print(random.choice(a, e), random.sample(a, [1], 2), len(unwrap(random.sample(a, [1, 2], 0))), bytes.len(random.bytes(a, 0)))']
first=m.int(0,1000000);expected.append(f'{first} {first} {m.int(0,7)}')
expected.append('err(RANDOM_EMPTY: choice requires non-empty input) err(RANDOM_RANGE: sample size exceeds input length) 0 0')
fips=["","abc","abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq","abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"]
for text in fips:program.append(f'    print(encoding.hex_encode(crypto.sha256(bytes.from_text("{text}"))))');expected.append(hashlib.sha256(text.encode()).hexdigest())
program.append('    print(encoding.hex_encode(crypto.sha256(bytes.from_text(strings.repeat("a", 1000000)))))');expected.append(hashlib.sha256(b'a'*1000000).hexdigest())
rfc4231=[(b'\x0b'*20,b'Hi There'),(b'Jefe',b'what do ya want for nothing?'),(b'\xaa'*20,b'\xdd'*50),(bytes(range(1,26)),b'\xcd'*50),(b'\x0c'*20,b'Test With Truncation'),
    (b'\xaa'*131,b'Test Using Larger Than Block-Size Key - Hash Key First'),
    (b'\xaa'*131,b'This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.')]
for key,data in rfc4231:
    program.append(f'    print(encoding.hex_encode(crypto.hmac_sha256(unwrap(encoding.hex_decode("{key.hex()}")), unwrap(encoding.hex_decode("{data.hex()}")))))')
    expected.append(hmac.new(key,data,hashlib.sha256).hexdigest())
program+=['    print(crypto.equal(bytes.from_text("ab"), bytes.from_text("ab")), crypto.equal(bytes.from_text("ab"), bytes.from_text("ac")), crypto.equal(bytes.from_text("a"), bytes.from_text("ab")), crypto.equal(bytes.from_text(""), bytes.from_text("")))']
expected.append('true false false true')
program.append('}');source='\n'.join(program)+'\n';expected='\n'.join(expected)+'\n'

with tempfile.TemporaryDirectory(prefix='hua-random-crypto-',dir=build) as temp:
    base=Path(temp);src=base/'source';src.mkdir();work=base/'work';work.mkdir();n=0
    def fixture(text):
        global n
        n+=1;p=src/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def triple(text,want):
        p=fixture(text);run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.unlink();run('check',artifact,expected='OK\n')
            run(command,target,cwd=work,expected=want)
        return artifact
    def failure(text,error,static=False):
        p=fixture(text)
        if static:run('check',p,code=1,error=error);return
        run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:run(command,target,cwd=work,code=1,error=error)
    artifact=triple(source,expected)
    assert header_format(artifact)==13,header_format(artifact)
    old=triple('import std.encoding as encoding\nimport std.bytes as bytes\nfn main() { print(encoding.hex_encode(bytes.from_text("a"))) }\n','61\n')
    assert header_format(old)<13,header_format(old)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,12);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    run('run',bad,code=1,error='E6002')
    # OS randomness: lengths, distinct draws and the UUID v4 layout on every path.
    p=fixture('''import std.crypto as crypto
import std.bytes as bytes
import std.encoding as encoding
fn main() {
    let a = encoding.hex_encode(unwrap(crypto.random_bytes(32)))
    let b = encoding.hex_encode(unwrap(crypto.random_bytes(32)))
    print(len(a), a != b, bytes.len(unwrap(crypto.random_bytes(0))))
    let u = unwrap(crypto.uuid4())
    let v = unwrap(crypto.uuid4())
    print(len(u), u != v)
    print(u)
}
''')
    artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
    for command,target in [('interpret',p),('run',p),('run',artifact)]:
        lines=run(command,target,cwd=work).strip().splitlines()
        assert lines[0]=='64 true 0' and lines[1]=='36 true',lines
        assert re.fullmatch(r'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}',lines[2]),lines[2]
    # Programmer errors and limits.
    failure('import std.random as random\nfn main() {\n    var r = random.new(1)\n    print(random.int(r, 5, 5))\n}\n','E4003')
    failure('import std.random as random\nfn main() {\n    var r = random.new(1)\n    print(random.bytes(r, -1))\n}\n','E4003')
    failure('import std.random as random\nfn main() {\n    var r = random.new(1)\n    print(random.sample(r, [1, 2], -1))\n}\n','E4003')
    failure('import std.crypto as crypto\nfn main() { print(crypto.random_bytes(1048577)) }\n','E4099')
    failure('import std.random as random\nfn main() {\n    var a = random.new(1)\n    var b = random.new(1)\n    print(a == b)\n}\n','E4008')
    # Static: read-only Rng/List, wrong argument kinds, opaque construction.
    failure('import std.random as random\nfn main() {\n    let r = random.new(1)\n    print(random.int(r, 0, 2))\n}\n','E3003',True)
    failure('import std.random as random\nfn draw(r random.Rng) int { return random.int(r, 0, 2) }\nfn main() { print(draw(random.new(1))) }\n','E3003',True)
    failure('import std.random as random\nimport std.list as list\nfn main() {\n    var r = random.new(1)\n    let l = list.from_slice([1, 2])\n    random.shuffle(r, l)\n}\n','E3003',True)
    failure('import std.random as random\nfn main() {\n    var r = random.new(1)\n    random.shuffle(r, [1, 2])\n}\n','E3004',True)
    failure('import std.random as random\nfn main() {\n    var r = random.new(1)\n    print(random.choice(r, 3))\n}\n','E3004',True)
    failure('import std.random as random\nfn main() { print(random.Rng{}) }\n','E3004',True)
    failure('import std.crypto as crypto\nfn main() { print(crypto.sha256("abc")) }\n','E3004',True)
print(f'random/crypto three-path checks passed: {checks}')
