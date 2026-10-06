# Binary files and collection invariants across three independent execution paths.
from pathlib import Path
import shutil,struct,subprocess,sys,tempfile,zlib
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0

def run(command,path,*extra,cwd=None,expected=None,code=0,error=None):
    global checks
    p=subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=cwd,capture_output=True,timeout=40)
    out=p.stdout.decode('utf-8').replace('\r\n','\n');err=p.stderr.decode('utf-8')
    assert p.returncode==code,(command,path,p.returncode,out,err)
    if expected is not None:assert out==expected,(command,out,expected,err)
    if error:assert error in err,(command,out,err,error)
    checks+=1;return out

with tempfile.TemporaryDirectory(prefix='hua-binary-',dir=build) as temp:
    base=Path(temp);source=base/'source';source.mkdir();work=base/'work';work.mkdir();n=0
    def fixture(text):
        global n
        n+=1;p=source/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def triple(text,expected,verify=None):
        p=fixture(text);run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.unlink();run('check',artifact,expected='OK\n')
            run(command,target,cwd=work,expected=expected)
            if verify:verify()
        return artifact
    def failure(text,error):
        p=fixture(text)
        for command in ['check','run','interpret','build']:run(command,p,code=1,error=error)
        assert not p.with_suffix('.huab').exists()
    triple('''import std.bytes as b
let raw=unwrap(b.from_ints([0,127,128,255]))
print(type(raw),b.len(raw),unwrap(b.to_ints(raw)),is_err(b.to_text(raw)))
print(unwrap(b.at(raw,2)),unwrap(b.to_ints(unwrap(b.slice(raw,1,3)))))
print(is_err(b.at(raw,-1)),is_err(b.at(raw,4)),is_err(b.slice(raw,2,1)),is_err(b.slice(raw,0,5)))
print(is_err(b.from_ints([-1])),is_err(b.from_ints([256])),b.len(unwrap(b.from_ints([]))))
print(unwrap(b.to_text(b.from_text("中文\\0"))),b.len(b.from_text("中文")))
print(unwrap(b.to_ints(unwrap(b.concat(raw,b.from_text("x"))))))
print(b.crc32(b.from_text("123456789")),b.crc32(b.from_text("")))
''','Bytes 4 [0, 127, 128, 255] true\n128 [127, 128]\ntrue true true true\ntrue true 0\n中文\0 6\n[0, 127, 128, 255, 120]\n3421780262 0\n')
    triple('''import std.bytes as b
print(unwrap(b.to_ints(unwrap(b.u32_le(4294967295)))))
print(unwrap(b.read_u32_le(unwrap(b.u32_le(4294967295)),0)))
print(unwrap(b.read_i64_le(b.i64_le(-9223372036854775808),0)),unwrap(b.read_i64_le(b.i64_le(9223372036854775807),0)))
print(unwrap(b.to_ints(b.i64_le(-1))))
print(is_err(b.u32_le(-1)),is_err(b.u32_le(4294967296)),is_err(b.read_u32_le(b.from_text("abc"),0)),is_err(b.read_i64_le(b.i64_le(0),1)))
''','[255, 255, 255, 255]\n4294967295\n-9223372036854775808 9223372036854775807\n[255, 255, 255, 255, 255, 255, 255, 255]\ntrue true true true\n')
    triple('''import std.buffer as buf
import std.bytes as b
var x=buf.new()
print(unwrap(buf.write_text(x,"Hua中文")))
let old=buf.bytes(x)
var independent=clone(x)
print(unwrap(buf.write_u8(x,255)),unwrap(buf.write_u32_le(x,16909060)),unwrap(buf.write_i64_le(x,-1)))
print(b.len(old),buf.len(independent),buf.len(x),is_err(buf.text(x)))
print(is_err(buf.write_u8(x,-1)),is_err(buf.write_u8(x,256)),is_err(buf.write_u32_le(x,4294967296)),buf.len(x))
print(unwrap(b.read_u32_le(buf.bytes(x),10)),unwrap(b.read_i64_le(buf.bytes(x),14)))
buf.clear(x)
print(buf.len(x),unwrap(buf.text(x)),unwrap(b.to_text(old)))
print(unwrap(buf.append(x,b.from_text("ok"))),unwrap(buf.text(x)))
''','9\n10 14 22\n9 9 22 true\ntrue true true 22\n16909060 -1\n0  Hua中文\n2 ok\n')
    triple('''import std.list as list
var xs=list.from_slice([1,2])
var alias=xs
let old=list.snapshot(xs)
var independent=clone(xs)
print(type(xs),unwrap(list.append(alias,3)),list.len(xs),old,list.snapshot(independent))
print(unwrap(list.set(xs,0,9)),unwrap(list.get(xs,0)),unwrap(list.extend(xs,old)),list.snapshot(xs))
print(unwrap(list.pop(xs)),list.snapshot(xs))
print(is_err(list.get(xs,-1)),is_err(list.get(xs,99)),is_err(list.set(xs,99,0)),list.snapshot(xs))
print(is_err(list.reserve(xs,-1)),is_err(list.reserve(xs,1000001)))
unwrap(list.reserve(xs,10))
print(unwrap(list.reserve(xs,1)))
list.clear(xs)
print(list.len(alias),is_err(list.pop(xs)),old)
var empty []int=[]
var values List<int> =list.from_slice(empty)
print(unwrap(list.append(values,7)),list.snapshot(values))
''','List<int> 3 3 [1, 2] [1, 2]\ntrue 9 5 [9, 2, 3, 1, 2]\n2 [9, 2, 3, 1]\ntrue true true [9, 2, 3, 1]\ntrue true\nfalse\n0 true [1, 2]\n1 [7]\n')
    # List nodes and nested values: constructor clones; get/snapshot never expose writable shared handles.
    triple('''import std.list as list
struct Token { text string }
var tokens=list.from_slice([Token{text:"a"}])
unwrap(list.append(tokens,Token{text:"b"}))
let first=unwrap(list.get(tokens,0))
print(first.text,list.len(tokens))
var source=[[1]]
var nested=list.from_slice(source)
source[0][0]=8
print(list.snapshot(nested))
var copy=clone(unwrap(list.get(nested,0)))
copy[0]=7
print(copy,list.snapshot(nested))
''','a 2\n[[1]]\n[7] [[1]]\n')
    triple('''import std.buffer as b
import std.list as l
fn add(x mut Buffer,values mut List<string>){unwrap(b.write_text(x,"x"))
unwrap(l.append(values,"x"))}
var buffer=b.new()
var values=l.from_slice(["a"])
add(buffer,values)
print(unwrap(b.text(buffer)),l.snapshot(values))
''','x [a, x]\n')
    triple('''import std.list as l
import std.buffer as b
struct State { output Buffer
values List<int> }
fn State.add(){unwrap(b.write_u8(self.output,1))
unwrap(l.append(self.values,1))}
var s=State{output:b.new(),values:l.from_slice([0])}
s.add()
print(b.len(s.output),l.snapshot(s.values))
''','1 [0, 1]\n')
    # File paths remain relative to process cwd. Verify arbitrary bytes independently in Python.
    binary=bytes(range(256))*8;(work/'目录').mkdir();(work/'目录/input.bin').write_bytes(binary)
    def check_binary():
        assert (work/'目录/output.bin').read_bytes()==binary+b'\x00\xff'
    triple('''import std.fs as fs
import std.bytes as b
let data=unwrap(fs.read_bytes("目录/input.bin"))
print(b.len(data),b.crc32(data),is_err(fs.read_text("目录/input.bin")))
print(unwrap(fs.write_bytes("目录/output.bin",data)),unwrap(fs.append_bytes("目录/output.bin",unwrap(b.from_ints([0,255])))))
print(b.len(unwrap(fs.read_bytes("目录/output.bin"))))
print(is_err(fs.read_bytes("missing")),is_err(fs.read_bytes(".")),is_err(fs.write_bytes(".",data)),is_err(fs.append_bytes("bad\\0path",data)))
''',f'2048 {zlib.crc32(binary)} true\ntrue true\n2050\ntrue true true true\n',check_binary)
    # Strict conversion and binary size boundaries, without allocating an oversized file in memory.
    (work/'large.bin').write_bytes(b'x'*(16*1024*1024+1))
    with (work/'oversized.bin').open('wb') as f:f.seek(128*1024*1024);f.write(b'x')
    triple('''import std.fs as fs
import std.bytes as b
let large=unwrap(fs.read_bytes("large.bin"))
print(b.len(large),is_err(b.to_text(large)),is_err(b.to_ints(large)),is_err(fs.read_bytes("oversized.bin")))
''','16777217 true true true\n')
    failure('import std.list as l\nlet x=l.from_slice([1])\nl.append(x,2)\n','E3003')
    failure('import std.buffer as b\nlet x=b.new()\nb.write_u8(x,1)\n','E3003')
    failure('import std.list as l\nvar x=l.from_slice([1])\nl.append(x,"bad")\n','E3004')
    failure('import std.list as l\nvar x=l.from_slice([1])\nl.extend(x,["bad"])\n','E3004')
    failure('import std.list as l\nl.from_slice([])\n','E3004')
    failure('import std.list as l\nl.get(1,0)\n','E3004')
    failure('import std.buffer as b\nb.len(1)\n','E3004')
    failure('import std.bytes as b\nb.from_ints([1.0])\n','E3004')
    failure('import std.list as l\nvar x List<int> =l.from_slice(["x"])\n','E3004')
    failure('var x List<void> =nil\n','E3004')
    for name in ['Bytes','Buffer','List']:failure('struct '+name+' {}\n','E3002')
    failure('import std.buffer as b\nfn bad(x Buffer){b.write_u8(x,1)}\n','E3003')
    failure('import std.list as l\nlet x=l.from_slice([1])\nvar y=x\nl.append(y,2)\n','E3003')
    failure('import std.buffer as b\nlet x=b.new()\nvar y=unwrap(ok(x))\nb.write_u8(y,1)\n','E3003')
    failure('import std.buffer as b\nimport std.fs as f\nf.write_bytes("must-not-exist",b.bytes(b.new()))\nmissing()\n','E3001')
    assert not (root/'must-not-exist').exists()
    # Unknown argument types must not circumvent mutability/type checks at runtime or in HUAB.
    for body,error in [('l.append(x,2)','E4007'),('l.append(x,"bad")','E4003')]:
        setup='let x=l.from_slice([1])' if error=='E4007' else 'var x=l.from_slice([1])'
        p=fixture('import std.list as l\nfn identity(v){return v}\n'+setup.replace('x=','original=')+'\nvar x=identity(original)\n'+body+'\n')
        run('build',p);out=p.with_suffix('.huab')
        for command,target in [('interpret',p),('run',p),('run',out)]:run(command,target,code=1,error=error)
    failure('''import std.list as l
struct State { values List<int> }
fn State.add(){l.append(self.values,1)}
let s=State{values:l.from_slice([0])}
s.add()
''','E3003')
    triple("""import std.buffer as b
import std.bytes as bytes
import std.list as l
import std.json as j
var x=b.new()
unwrap(b.write_text(x,"x"))
var alias=x
var isolated=clone(x)
b.clear(alias)
print(b.len(x),b.len(isolated),unwrap(b.text(isolated)))
let values=l.from_slice([isolated])
print(b.len(unwrap(l.get(values,0))),is_err(j.encode(values)),is_err(j.encode(bytes.from_text("x"))),is_err(j.encode(x)))
""",'''0 1 x
1 true true true
''')
    failure('''import std.buffer as b
var x=b.new()
let y=b.new()
x=y
b.clear(x)
''','E3003')
    failure('''import std.buffer as b
let y=b.new()
var x,z=y,1
b.clear(x)
''','E3003')
    failure('''import std.list as l
import std.buffer as b
var x=l.from_slice([b.new()])
var item=unwrap(l.get(x,0))
b.clear(item)
''','E3003')
    failure('''import std.list as l
import std.buffer as b
var x=l.from_slice([b.new()])
var item=l.snapshot(x)[0]
b.clear(item)
''','E3003')
    # Boundaries execute actual buffer/list growth, and failures preserve size/content.
    triple("""import std.fs as fs
import std.bytes as bytes
import std.buffer as b
import std.list as l
let raw=unwrap(fs.read_bytes("large.bin"))
let block=unwrap(bytes.slice(raw,0,16777216))
var out=b.new()
for i in 0..8 { unwrap(b.append(out,block)) }
let all=b.bytes(out)
print(b.len(out),is_err(b.append(out,bytes.from_text("x"))),b.len(out))
print(is_err(bytes.concat(all,bytes.from_text("x"))),is_err(b.text(out)))
let items=unwrap(bytes.to_ints(unwrap(bytes.slice(raw,0,1000000))))
var xs=l.from_slice(items)
print(l.len(xs),is_err(l.append(xs,120)),is_err(l.extend(xs,[120])),l.len(xs))
print(unwrap(l.pop(xs)),unwrap(l.append(xs,120)),l.len(xs))
""",'''134217728 true 134217728
true true
1000000 true true 1000000
120 1000000 1000000
''')
    triple('import std.list as l\nfn make(xs){var out=l.from_slice(xs)\nunwrap(l.append(out,3))\nreturn l.snapshot(out)}\nprint(make([1,2]))\n','[1, 2, 3]\n')
    failure('import std.list as l\nvar xs=l.from_slice([1.0])\nl.append(xs,1)\n','E3004')
    # v4 new IDs rejected in every older format; original v3 standard IDs remain readable.
    p=fixture('import std.bytes as b\nprint(b.len(b.from_text("x")))\n');run('build',p);original=p.with_suffix('.huab').read_bytes();assert struct.unpack_from('<II',original,8)==(6,1)
    for version in [1,2,3]:
        bad=bytearray(original);struct.pack_into('<I',bad,8,version);out=base/f'bad-v{version}.huab';out.write_bytes(bad);run('check',out,code=1,error='E6002')
    p=fixture('import std.strings as s\nprint(s.upper_ascii("hua"))\n');run('build',p);old=bytearray(p.with_suffix('.huab').read_bytes());struct.pack_into('<I',old,8,3);out=base/'valid-v3.huab';out.write_bytes(old);run('check',out,expected='OK\n');run('run',out,expected='HUA\n')
    # Imported List<T> and Buffer signatures survive metadata serialization and deleted modules.
    (source/'library.hua').write_text('import std.list as l\nimport std.buffer as b\npub fn sizes(x List<int>,out Buffer) int{return l.len(x)+b.len(out)}\npub struct Token { kind int }\npub fn tokens() List<Token>{return l.from_slice([Token{kind:1}])}\n',encoding='utf-8')
    p=fixture('import library as lib\nimport std.list as l\nimport std.buffer as b\nprint(lib.sizes(l.from_slice([1,2]),b.new()),unwrap(l.get(lib.tokens(),0)).kind)\n');run('check',p,expected='OK\n');run('interpret',p,expected='2 1\n');run('run',p,expected='2 1\n');out=base/'module.huab';run('build',p,'-o',out);shutil.rmtree(source);run('check',out,expected='OK\n');run('run',out,expected='2 1\n')
print(f'{checks} binary/list/buffer CLI checks passed (Interpreter, VM, source-free HUAB; independent Python binary/CRC checks)')
