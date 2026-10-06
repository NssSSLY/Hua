# Real stdlib calls across Interpreter, source VM and source-free HUAB.
from pathlib import Path
import json,shutil,struct,subprocess,sys,tempfile,zlib
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0

def run(command,path,*extra,cwd=None,input=b'',expected=None,code=0,error=None):
    global checks
    result=subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=cwd,input=input,capture_output=True,timeout=15)
    out=result.stdout.decode('utf-8').replace('\r\n','\n');err=result.stderr.decode('utf-8')
    assert result.returncode==code,(command,path,result.returncode,out,err)
    if expected is not None:assert out==expected,(command,path,out,expected,err)
    if error:assert error in err and not out,(command,out,err,error)
    checks+=1;return out

with tempfile.TemporaryDirectory(prefix='hua-stdlib-',dir=build) as temp:
    base=Path(temp);source=base/'source';source.mkdir();work=base/'work';work.mkdir();n=0
    def fixture(text):
        global n
        n+=1;p=source/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def triple(text,expected,input=b'',arguments=()):
        p=fixture(text);run('check',p,expected='OK\n');run('build',p);artifact=p.with_suffix('.huab')
        for command in ['interpret','run']:run(command,p,'--',*arguments,cwd=work,input=input,expected=expected)
        p.unlink();run('check',artifact,expected='OK\n');run('run',artifact,'--',*arguments,cwd=work,input=input,expected=expected)
        return artifact
    def failure(text,error):
        p=fixture(text)
        for command in ['check','run','interpret','build']:run(command,p,code=1,error=error)
        assert not p.with_suffix('.huab').exists()
    triple('import std.os as os\nimport std.io as io\nprint(os.args())\nprint(unwrap(io.read_line()),unwrap(io.read_line()),unwrap(io.read_line()))\n',
           '[--flag, 中文, , a b]\nhello  nil\n',input=b'hello\r\n\n',arguments=['--flag','中文','','a b'])
    triple('import std.io\nprint(unwrap(std.io.read_all()))\n','中文\n\n',input='中文\n'.encode())
    triple('import std.io as io\nprint(is_err(io.read_all()))\n','true\n',input=b'\xc0\xaf')
    triple('import std.io as io\nlet x=io.write("a\\0b")\nprint(unwrap(x))\n','a\0b3\n')
    triple('import std.os as os\nprint(unwrap(os.cwd()))\n',str(work)+'\n')
    triple(r'''import std.fs as fs
import std.json as json
import std.strings as strings
fn workflow() Result<string> {
unwrap(fs.mkdir_all("目录"))
let path="目录/data.json"
fs.write_text(path,"{\"name\":\"Hua\",\"count\":3}")?
fs.append_text(path,"\n")?
let value=json.decode(fs.read_text(path)?)?
let name=json.as_string(json.get(value,"name")?)?
let count=json.as_int(json.get(value,"count")?)?
print(fs.exists(path),unwrap(fs.list_dir("目录")))
fs.remove(path)?
fs.remove("目录")?
return ok(strings.upper_ascii(name)+":"+str(count))
}
print(workflow())
''','ok(true) [data.json]\nok(HUA:3)\n')
    assert not (work/'目录').exists()
    (work/'invalid.txt').write_bytes(b'\xff');(work/'original.txt').write_bytes(b'original')
    triple(r'''import std.fs as fs
print(is_err(fs.read_text("missing")),is_err(fs.read_text("invalid.txt")),is_err(fs.read_text(".")))
print(is_err(fs.write_text(".","x")),is_err(fs.write_text("bad\0path","x")))
print(unwrap(fs.remove("missing")),unwrap(fs.mkdir_all(".")))
''','true true true\ntrue true\nfalse false\n')
    assert (work/'original.txt').read_bytes()==b'original'
    raw=b'\xef\xbb\xbfhello\r\n\x00end';(work/'raw.txt').write_bytes(raw)
    triple('import std.fs as fs\nimport std.strings as s\nprint(s.len_bytes(unwrap(fs.read_text("raw.txt"))))\n',str(len(raw))+'\n')
    triple(r'''import std.strings as s
print(s.contains("Hua 中文","中文"),s.starts_with("Hua","Hu"),s.ends_with("Hua","a"),s.index("中文Hua","Hua"),s.index("Hua","x"))
print(s.trim(" \tHua\r\n"),s.lower_ascii("HUA中文"),s.upper_ascii("hua中文"))
print(s.len_bytes("中文"),unwrap(s.rune_count("中文")),unwrap(s.slice("中文Hua",6,9)),is_err(s.slice("中文",1,3)))
print(s.split("a,,b,",","),s.split("中文", ""),s.split("", ""),s.join(["a","b"],"-"))
print(s.replace("aaaa","aa","b"),s.replace("abc","","x"))
''','true true true 6 -1\nHua hua中文 HUA中文\n6 2 Hua true\n[a, , b, ] [中, 文] [] a-b\nbb abc\n')
    triple(r'''import std.json as j
struct Person { name string
age int }
let v j.Value = unwrap(j.decode("{\"x\":null,\"items\":[true,2,1.5]}"))
print(type(v),j.kind(v),unwrap(j.keys(v)),unwrap(j.len(v)))
print(j.is_null(unwrap(j.get(v,"x"))),is_err(j.get(v,"missing")))
let a=unwrap(j.get(v,"items"))
print(unwrap(j.len(a)),unwrap(j.as_bool(unwrap(j.at(a,0)))),unwrap(j.as_int(unwrap(j.at(a,1)))),unwrap(j.as_float(unwrap(j.at(a,2)))))
print(is_err(j.at(a,-1)),is_err(j.at(a,3)),is_err(j.as_string(a)),is_err(j.len(unwrap(j.decode("1")))))
print(unwrap(j.encode(Person{name:"Hua",age:3})),unwrap(j.encode(map[string]int{"b":2,"a":1})))
print(is_err(j.encode(map[int]int{1:2})),is_err(j.encode(ok(1))))
print(is_err(j.as_float(unwrap(j.decode("9007199254740993")))))
''','Json object [items, x] 2\ntrue true\n3 true 2 1.5\ntrue true true true\n{"age":3,"name":"Hua"} {"a":1,"b":2}\ntrue true\ntrue\n')
    failure('import std.fs as fs\nfs.read_text(1)\n','E3004')
    failure('import std.strings as s\ns.split("x")\n','E3005')
    failure('import std.json as j\nj.as_int(1)\n','E3004')
    failure('import std.unknown\n','E5001')
    failure('import std.fs as fs\nfs.unknown()\n','E5003')
    p=fixture('import std.strings as s\nprint(s.len_bytes("A"))\n')
    run('run',p,'arg',code=2,error='Usage:');run('check',p,'--',code=2,error='Usage:')
    (source/'std').mkdir();(source/'std/strings.hua').write_text('pub fn len_bytes(x string) int{return 999}\n',encoding='utf-8')
    run('run',p,expected='1\n');run('build',p);artifact=p.with_suffix('.huab');original=artifact.read_bytes()
    assert struct.unpack_from('<II',original,8)==(6,1)
    for version in [1,2]:
        bad=bytearray(original);struct.pack_into('<I',bad,8,version);path=base/f'old{version}.huab';path.write_bytes(bad)
        run('check',path,code=1,error='E6002')
    bad=bytearray(original);target=b'$core$$std$strings$len_bytes';at=bad.rindex(target);bad[at:at+len(target)]=target[:-1]+b'x'
    struct.pack_into('<I',bad,24,zlib.crc32(bad[32:]));path=base/'unknown.huab';path.write_bytes(bad);run('check',path,code=1,error='E6002')
    triple('import std.io as io\nimport std.json as j\nprint(unwrap(j.encode(unwrap(io.read_line()))))\n','"x\\u000d"\n',input=b'x\r')
    triple('import std.strings as a\nimport std.strings as b\nlet f=a.upper_ascii\nprint(f("hua"),b.upper_ascii("vm"))\n','HUA VM\n')
    (work/'large.txt').write_bytes(b'x'*(16*1024*1024+1))
    triple('import std.fs as fs\nprint(is_err(fs.read_text("large.txt")))\n','true\n')
    triple('import std.io as io\nprint(is_err(io.read_all()))\n','true\n',input=b'x'*(16*1024*1024+1))
    failure('struct Json {}\n','E3002')
    failure('import std.fs as fs\nfs.write_text("must_not_exist","x")\nmissing()\n','E3001')
    assert not (root/'must_not_exist').exists()
    # Source-free standard aliases also retain static types and readonly slices.
    failure('import std.os as os\nvar args=os.args()\nargs[0]="bad"\n','E3003')
    p=fixture('var m=map[string]int{"x":2}\nprint(unwrap(m["x"]))\n');run('build',p);legacy=bytearray(p.with_suffix('.huab').read_bytes())
    struct.pack_into('<I',legacy,8,2);path=base/'v2.huab';path.write_bytes(legacy);p.unlink();run('check',path,expected='OK\n');run('run',path,expected='2\n')
    library=source/'helper.hua';library.write_text('import std.json as j\npub fn parse(text string) Result<j.Value> {return j.decode(text)}\n',encoding='utf-8',newline='\n')
    p=fixture('import helper\nimport std.json as j\nlet v=unwrap(helper.parse("[1,2]"))\nprint(unwrap(j.len(v)))\n')
    run('check',p,expected='OK\n')
    for command in ['run','interpret']:run(command,p,expected='2\n')
    artifact=base/'module.huab';run('build',p,'-o',artifact);p.unlink();library.unlink();run('check',artifact,expected='OK\n');run('run',artifact,expected='2\n')
    p=fixture('''import std.io as io
import std.json as j
fn parse() Result<string> { let text=io.read_all()?
return j.encode(j.decode(text)?) }
fn main(){let r=parse()
if is_err(r){print("ERR")}else{print(unwrap(r))}}
''')
    valid=['null','true','false','0','-0','-9223372036854775808','9223372036854775807','1.0','-1.25e+3',
           '"中文😀"','"\\uD83D\\uDE00"','"\\u0000\\b\\f\\n\\r\\t\\\\\\/\\\""','[]','{}',
           '{"b":[true,null,"中"],"a":{"x":2}}','{"a":1,"a":2}',' \r\n[1,2,3]\t']
    invalid=['','[','{"a"}','{"a":1,}','[1,]','[1 2]','true false','undefined','NaN','Infinity','01','-01','+1','1.','1e','1e+','1e9999',
             '9223372036854775808','-9223372036854775809','"\\x00"','"\\uD800"','"\\uDC00"','"\\uD800\\u0041"','"\n"','/* x */1','{a:1}','\ufeff1',
             '['*65+'0'+']'*65]
    artifact=base/'corpus.huab';run('check',p,expected='OK\n');run('build',p,'-o',artifact)
    for command,target in [('run',p),('interpret',p),('run',artifact)]:
        if target==artifact:shutil.rmtree(source)
        for text in valid:
            output=run(command,target,input=text.encode(),cwd=work);assert json.loads(output)==json.loads(text),(text,output)
        for text in invalid:run(command,target,input=text.encode(),cwd=work,expected='ERR\n')
print(f'{checks} stdlib/input/JSON CLI checks passed (3 execution paths, source-free HUAB, real files/stdin/args)')
