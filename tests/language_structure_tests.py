"""Phase 9: source/check/interpreter/VM/persisted language and type contracts."""
import pathlib, subprocess, sys, tempfile, struct, zlib
exe=pathlib.Path(sys.argv[1]);build=pathlib.Path(sys.argv[2]);checks=0;cases=0

def run(command,path,code=0,output=None,error=None):
    global checks
    result=subprocess.run([str(exe),command,str(path)],capture_output=True,encoding="utf-8",timeout=20)
    assert result.returncode==code,(command,path,result.stdout,result.stderr)
    if output is not None: assert result.stdout==output,(command,path,result.stdout,output,result.stderr)
    if error: assert error in result.stderr,(command,path,result.stderr,error)
    checks+=1;return result
with tempfile.TemporaryDirectory(prefix="hua-language-",dir=build) as directory:
    root=pathlib.Path(directory)
    def fixture(text,modules=None):
        global cases
        cases+=1;p=root/str(cases);p.mkdir()
        for name,source in (modules or {}).items():(p/name).write_text(source,encoding="utf-8")
        entry=p/"main.hua";entry.write_text(text,encoding="utf-8");return entry
    def triple(text,output,modules=None):
        global checks
        entry=fixture(text,modules);run("check",entry,output="OK\n")
        for engine in ("interpret","run"):run(engine,entry,output=output)
        run("build",entry);archive=entry.with_suffix(".huab");assert struct.unpack_from("<II",archive.read_bytes(),8)==(6,1);checks+=1
        for source in entry.parent.glob("*.hua"):source.unlink()
        run("check",archive,output="OK\n");run("run",archive,output=output)
        return archive
    def invalid(text,error="E3004",modules=None):
        entry=fixture(text,modules)
        for command in ("check","interpret","run","build"):run(command,entry,code=1,error=error)
    def runtime_failure(text,error,output=""):
        entry=fixture(text);run("check",entry,output="OK\n")
        for engine in ("interpret","run"):run(engine,entry,code=1,error=error,output=output)
        run("build",entry);entry.unlink();run("run",entry.with_suffix(".huab"),code=1,error=error,output=output)

    triple("""fn id<T>(x T) T {return x}
struct Box<T>{ value T }
@entry
fn start(){
 print(id<int>(4),id<string>("Hua"))
 let box Box<int>=Box<int>{value:7}
 print(box.value)
 var x=1
 fn captured() int {return x}
 x=9
 print(captured())
 { struct Local{value int}
 let v=Local{value:3}
 print(v.value)
 }
 defer print("end",x)
 {defer print("block")}
 defer print("last")
}
""","4 Hua\n7\n1\n3\nblock\nlast\nend 9\n")
    triple("""interface Named { fn name() string }
struct A {value string}
fn A.name() string {return self.value}
fn name(value Named) string {return value.name()}
enum Event {Text(string), Number(int), Empty}
fn describe(value Event) string {
 match value {
 Text(s)=>{return s}
 Number(n)=>{return str(n)}
 Empty=>{return "empty"}
 }
}
print(name(A{value:"interface"}))
print(describe(Event.Text("text")),describe(Event.Number(7)),describe(Event.Empty()))
match true {true=>print("yes")
false=>print("no")}
""","interface\ntext 7 empty\nyes\n")
    triple('import api\nlet item api.Box<int>=api.Box<int>{value:5}\nprint(api.id<int>(item.value))\nprint(api.show(api.Item{value:"public"}))\nmatch api.Event.Text("event"){api.Event.Text(x)=>print(x)\napi.Event.Empty=>print("empty")}\n',"5\npublic\nevent\n",{"api.hua":"""pub struct Box<T>{value T}
pub fn id<T>(x T) T {return x}
pub interface Named {fn name() string}
pub struct Item{value string}
pub fn Item.name() string{return self.value}
pub fn show(x Named) string{return x.name()}
pub enum Event{Text(string), Empty}
"""})
    triple('fn outer(){var x=3\nfn recur(n int) int {if n==0{return x}\nreturn recur(n-1)}\nreturn recur}\nlet closure=outer()\nprint(closure(4))\n',"3\n")
    triple('fn f() int{defer print("return")\nreturn 5}\nfn main(){for i in 0..3 {defer print(i)\nif i==1{continue}\nif i==2{break}}\nprint(f())}\n',"0\n1\n2\nreturn\n5\n")
    runtime_failure('fn failed(){defer print("inner")\nprint(1//0)}\nfn main(){defer print("outer")\ndefer failed()\nprint(2//0)}\n',"E4004","inner\nouter\n")
    triple("""fn owned() []int{return [1,2]}
fn forward() []int{return owned()}
fn main(){
 var a [3]u16=[1,2,65535]
 var b=a
 b[0]=9
 var view=a[1:]
 view[0]=7
 print(a,b,view,sizeof(a),alignof(a),type(a),type(view))
 var matrix [2][2]i16=[[1,2],[3,4]]
 matrix[0][1]=8
 print(matrix,sizeof(matrix),alignof(matrix))
 var v=forward()
 v[0]=8
 print(v)
 var small i8=127
 small-=1
 let large=u64("18446744073709551615")
 var real f32=1.5
 real+=0.25
 print(small,large,real,type(small),type(large),type(real),sizeof(small),sizeof(large),sizeof(real))
}
""","[1, 7, 65535] [9, 2, 65535] [7, 65535] 6 2 [3]u16 []u16\n[[1, 8], [3, 4]] 8 2\n[8, 2]\n126 18446744073709551615 1.75 i8 u64 f32 1 8 4\n")
    triple('import std.list as l\nimport std.json as j\nvar a [2]int=[1,2]\nvar view=a[:]\nprint(unwrap(j.encode(a)))\nvar xs=l.from_slice(a)\nprint(unwrap(l.get(xs,1)))\nfn mutate(v mut [2]int){v[0]=8}\nmutate(a)\nprint(a,view)\n',"[1,2]\n2\n[8, 2] [8, 2]\n")
    for t,low,high in [("i8",-128,127),("i16",-32768,32767),("i32",-2147483648,2147483647),("i64",-9223372036854775808,9223372036854775807),("u8",0,255),("u16",0,65535),("u32",0,4294967295),("u64",0,18446744073709551615),("byte",0,255),("usize",0,18446744073709551615),("isize",-9223372036854775808,9223372036854775807)]:
        triple(f'let low={t}("{low}")\nlet high={t}("{high}")\nprint(low,high,type(high),sizeof(high))\n',f'{low} {high} {t} '+str({"byte":1,"i8":1,"u8":1,"i16":2,"u16":2,"i32":4,"u32":4}.get(t,8))+"\n")
        runtime_failure(f'var x={t}("{high}")\nx+=1\n',"E4002")
    triple('const value i8=3\nprint(type(value),value+1)\nprint(i32(i8(2)),f64(f32(1.5)),sqrt(u8(9)),abs(i8(-2)))\n',"i8 4\n2 1.5 3 2\n")
    triple('fn guard(x int?) int{if is_none(x){return 0}\nreturn x+1}\nfn main(){var x int?=2\nif x!=nil&&x>1{print(x+1)}\nif x==nil{x=3}else{x=4}\nprint(x+1,guard(nil),guard(7))\nwhile x!=nil{print(x+1)\nx=nil}\n}\n',"3\n5 0 8\n5\n")
    triple('fn borrow(v mut []int) []int{return v}\nfn forward(v mut []int) []int{return borrow(v)}\nfn main(){var a=[1]\nvar b=forward(a)\nb[0]=7\nprint(a)}\n',"[7]\n")
    invalid('fn f(v []int) []int{return v}\nfn main(){var a=f([1])\na[0]=2}\n',"E3003")
    invalid('fn main(){let a=[1]\nfn write(){a[0]=2}\nwrite()}\n',"E3003")
    invalid('fn main(){var x int?=2\nif x!=nil{x=nil\nprint(x+1)}}\n')
    invalid('fn main(){var x int?=1\nwhile x!=nil{print(x+1)\nx=nil}\nprint(x+1)}\n')
    invalid('fn main(){var x int?=1\nfor i in 0..2{print(x+1)\nx=nil}}\n')
    invalid('interface I{fn value() int}\nstruct S{}\nfn S.value() string{return "bad"}\nfn f(x I){}\nf(S{})\n')
    invalid('interface I{fn set()}\nstruct S{n int}\nfn write(s mut S){s.n=1}\nfn S.set(){write(self)}\nfn f(x I){}\nf(S{n:0})\n')
    triple('interface I{mut fn set()}\nstruct S{n int}\nfn write(s mut S){s.n=1}\nfn S.set(){write(self)}\nfn f(x mut I){x.set()}\nvar s=S{n:0}\nf(s)\nprint(s.n)\n',"1\n")
    for text,error in [
        ('enum E{A(int), B}\nmatch E.B(){A(x)=>print(x)}\n',"E3011"),
        ('enum E{A(int)}\nmatch E.A(1){A()=>print(1)}\n',"E3011"),
        ('enum E{A}\nmatch E.A(){A=>print(1)\nA=>print(2)}\n',"E3011"),
        ('enum E{A}\nmatch E.A(){_=>print(1)\nA=>print(2)}\n',"E3011"),
        ('match 1{1=>print(1)}\n',"E3011"),
        ('enum E{A(int)}\nE.A("x")\n',"E3004"),
        ('fn id<T>(x T) T{return x}\nid<int,string>(1)\n',"E3010"),
        ('fn id<T>(x T) T{return x}\nid<int>("x")\n',"E3004"),
        ('@unknown\nfn f(){}\n',"E2002"),
        ('@inline("x")\nfn f(){}\n',"E2002"),
        ('@inline\n@noinline\nfn f(){}\n',"E2002"),
        ('@entry\nfn f(x int){}\n',"E3010"),
        ('@entry\nfn f(){}\nfn main(){}\n',"E3010"),
        ('var x i8=1\nvar y i16=2\nx=y\n',"E3004"),
        ('var x u8=1\nvar y i8=2\nx=y\n',"E3004"),
        ('let a=[1]\nsizeof(a)\n',"E3004"),
        ('fn main(){enum E{A}}\n',"E2002"),
    ]:invalid(text,error)
    runtime_failure('var x i8=127\nx+=1\n',"E4002")
    runtime_failure('print(u8(-1))\n',"E4002")
    runtime_failure('var a [2]u8=[1,256]\n',"E4002")
    runtime_failure('var a [3]int=[1,2]\n',"E4003")
    runtime_failure('print(f32(1e300))\n',"E4002")
    runtime_failure('var x u8=1\nx=x<<8\n',"E4005")
    triple('@doc(" pub unsafe external")\n@deprecated("message")\nfn ordinary() int{return 3}\nprint(ordinary())\n',"3\n")
    triple('fn guard(x int?) int{match x{nil=>{return 0}\n_=>{return x+1}}}\nprint(guard(nil),guard(4))\n',"0 5\n")
    triple('fn main(){for i in 0..2{let x int?=i\nif x==nil{continue}\nprint(x+1)}}\n',"1\n2\n")
    triple('interface I{fn f() int}\ninterface J{fn f() int\nfn g() int}\nstruct S{}\nfn S.f() int{return 2}\nfn S.g() int{return 3}\nfn convert(x J) I{return x}\nprint(convert(S{}).f())\n',"2\n")
    triple('fn id<T>(x T) T{return x}\nstruct Box<T>{value T}\nlet value=Box<Box<int>>{value:Box<int>{value:8}}\nprint(id<Box<int>>(value.value).value)\n',"8\n")
    triple('fn owned<T>(value T) []T{return [value]}\nfn main(){var xs=owned<int>(1)\nxs[0]=3\nprint(xs)}\n',"[3]\n")
    triple('var a [0]u8=[]\nvar b [2]bool=[true,false]\nb[1]=true\nprint(a,b,sizeof(a),sizeof(b),alignof(b))\n',"[] [true, true] 0 2 1\n")
    triple('fn copy(a [2]int) [2]int{return a}\nvar a [2]int=[1,2]\nlet b=copy(a)\na[0]=9\nprint(a,b)\n',"[9, 2] [1, 2]\n")
    triple('var a [2][2]u8=[[1,2],[3,4]]\nvar view=a[0][:]\nvar copy=a[0]\nview[1]=5\ncopy[0]=9\nprint(a,view,copy)\n',"[[1, 5], [3, 4]] [1, 5] [9, 2]\n")
    triple('var a=[i16(1),i8(2)]\nprint(type(a[1]),sizeof(a[1]))\nprint(i8(127.9),u8(255.9))\n',"i16 2\n127 255\n")
    triple('import std.json as j\nenum E{A}\nprint(is_err(j.encode(E.A())),is_err(j.encode(u64("18446744073709551615"))))\n',"true true\n")
    invalid('fn f(v []int) []int{var result=v\n{var result=[1]}\nreturn result}\nfn main(){var a=f([1])\na[0]=3}\n',"E3003")
    invalid('fn main(){var x int?=1\nif x!=nil{x,_=nil,2\nprint(x+1)}}\n')
    invalid('fn main(){let readonly=[1]\nvar value=[2]\nvar n=0\nwhile n<2{value[0]=n\nvalue=readonly\nn+=1}}\n',"E3003")
    invalid('fn main(){let readonly=[1]\nvar value=[2]\nfor n in 0..2{value[0]=n\nvalue=readonly}}\n',"E3003")
    invalid('interface I{fn f() int}\nI.f()\n')
    runtime_failure('print(-i8(-128))\n',"E4002")
    runtime_failure('var x u8=0\nx-=1\n',"E4002")
    triple('print(i8(-7)//3,i8(-7)%3,u8(7)//3,u8(7)%3,u8(2)**7)\n',"-3 2 2 1 128\n")
    triple('fn recurse(n int) []int{if n==0{return [1]}\nreturn recurse(n-1)}\nfn main(){fn owned() []int{return [2]}\nvar a=owned()\na[0]=3\nvar b=recurse(4)\nb[0]=5\nprint(a,b)}\n',"[3] [5]\n")
    triple('struct S{}\nfn S.owned() []int{return [1]}\nfn forward(s S) []int{return s.owned()}\nvar a=forward(S{})\na[0]=4\nprint(a)\n',"[4]\n")
    invalid('const x i8=127\nconst y=x+1\n',"E4002")
    triple('let a=min(u8(2),1)\nlet b=max(i16(2),3)\nlet c=clamp(f32(2.5),1.5,2.0)\nprint(a,b,c,type(a),type(b),type(c))\n',"1 3 2 u8 i16 f32\n")
    runtime_failure('fn index(a,x){print(a[x])}\nindex([1,2],f32(0.5))\n',"E4003")
    triple('fn array() [2]u8{return [1,2]}\nfn head(a [2]u8) u8{return a[0]}\nvar a [2]u8=[3,4]\na=[5,6]\nprint(array(),head([7,8]),a)\n',"[1, 2] 7 [5, 6]\n")
    runtime_failure('fn unknown(){return 1.5}\nvar a [1]u8=[unknown()]\n',"E4003")
    triple('var a [2]u8,b [2]u8=[1,2],[3,4]\nvar table=map[string][2]u8{"x":[5,6]}\nlet result Result<[2]u8>=ok([7,8])\nprint(a,b,unwrap(table["x"]),unwrap(result))\n',"[1, 2] [3, 4] [5, 6] [7, 8]\n")
    invalid('fn mutate(v mut []i16){v[0]=i16(2)}\nvar v=[i8(1)]\nmutate(v)\n')
    invalid('fn mutate(v mut [1]int){v[0]=2}\nvar v=[1]\nmutate(v)\n')
    runtime_failure('fn source(){return [i8(1)]}\nfn mutate(v mut []i16){v[0]=i16(2)}\nvar v=source()\nmutate(v)\n',"E4003")
    for type_name,value,bits in (("i8",9,128),("f32",1.5,struct.unpack("<Q",struct.pack("<d",1.5000000001))[0])):
        archive=triple(f'const x {type_name}={value}\nprint(x)\n',f'{value}\n')
        original=archive.read_bytes();kind=0 if type_name=="i8" else 2
        marker=bytes([15])+struct.pack("<I",len(type_name))+type_name.encode()+bytes([kind])
        at=original.index(marker)+len(marker)
        modified=bytearray(original);struct.pack_into("<Q",modified,at,bits)
        payload=modified[32:];struct.pack_into("<I",modified,24,zlib.crc32(payload));archive.write_bytes(modified)
        for command in ("check","run"):run(command,archive,code=1,error="E6002")
    archive=triple('print("legacy")\n',"legacy\n");original=archive.read_bytes()
    for version in (1,2,3,4):
        changed=bytearray(original);struct.pack_into("<I",changed,8,version);archive.write_bytes(changed)
        run("check",archive,output="OK\n");run("run",archive,output="legacy\n")
print(f"{cases} language/type fixtures; {checks} interpreter/VM/persisted checks passed")
