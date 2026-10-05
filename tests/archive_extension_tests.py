"""Real persistence, Native and WASM tests; no mocks or interpreter fallback."""
from pathlib import Path
import shutil,struct,subprocess,sys,tempfile,zlib
exe,root,build,runtime=map(Path,sys.argv[1:])
sys.path.insert(0,str(root/"scripts"))
from make_wasm import compile_wat
checks=0

def run(command,path,*extra,code=0,output=None,error=None,silent=False):
    global checks
    result=subprocess.run([str(exe),command,str(path),*map(str,extra)],capture_output=True,timeout=15)
    result.stdout=result.stdout.decode("utf-8").replace("\r\n","\n").replace("\r","\n");result.stderr=result.stderr.decode("utf-8").replace("\r\n","\n").replace("\r","\n")
    assert result.returncode==code,(command,path,result.returncode,result.stdout,result.stderr)
    if output is not None: assert result.stdout==output,(command,result.stdout,output,result.stderr)
    if error is not None: assert error in result.stderr,(command,result.stderr,error)
    if silent: assert not result.stdout,result.stdout
    checks+=1
    return result

def records(data):
    pos=32
    def u32():
        nonlocal pos
        v=struct.unpack_from("<I",data,pos)[0];pos+=4;return v
    def byte():
        nonlocal pos
        v=data[pos];pos+=1;return v
    def string():
        nonlocal pos
        n=u32();s=data[pos:pos+n];pos+=n;return s
    def span():
        nonlocal pos
        pos+=20
    for _ in range(u32()): string();string()
    for _ in range(u32()):
        string();span();byte()
        for _ in range(u32()): string();string();span()
    for _ in range(u32()):
        string();span();byte();byte();byte();byte()
        for _ in range(u32()): string();string();byte();span()
        for _ in range(u32()): string()
    assert u32()==0
    def chunk():
        string();out=[]
        for _ in range(u32()):
            start=pos;op=byte();span();string();string();arg=pos;u32();target=pos;u32();byte();kind=byte()
            if kind==1:byte()
            elif kind in (2,3):u32();u32()
            elif kind==4:string()
            for _ in range(u32()):string()
            for _ in range(u32()):byte()
            out.append((start,op,arg,target))
        return out
    out=chunk()
    for _ in range(u32()):out+=chunk()
    return out

with tempfile.TemporaryDirectory(prefix="hua-artifacts-",dir=build) as temp:
    base=Path(temp)
    def case():
        p=base/str(checks);p.mkdir();return p
    suffix=".dll" if sys.platform=="win32" else ".dylib" if sys.platform=="darwin" else ".so"
    count=0
    for source in sorted((root/"tests/runtime").glob("*.hua")):
        expected=source.with_suffix(".expect").read_text(encoding="utf-8").splitlines()[0]
        if source.name=="142_import.hua":continue # Isolated unlinked API test.
        stdout=source.with_suffix(".stdout").read_text(encoding="utf-8") if source.with_suffix(".stdout").exists() else ""
        p=case();entry=p/"main.hua";entry.write_bytes(source.read_bytes());artifact=entry.with_suffix(".huab")
        if expected.startswith("CHECK_ERROR"):
            run("build",entry,code=1,error=expected.split()[1],silent=True)
            assert not artifact.exists();continue
        run("build",entry)
        assert artifact.read_bytes()[:8]==b"HUAB\r\n\x1a\n"
        assert struct.unpack_from("<II",artifact.read_bytes(),8)==(1,1)
        entry.unlink()
        run("check",artifact,output="OK\n")
        if expected=="OK":run("run",artifact,output=stdout)
        else:
            result=run("run",artifact,code=1,output=stdout,error=expected.split()[1])
            assert "main.hua:" in result.stderr and "^" in result.stderr
        count+=1
    p=case();shutil.copytree(root/"examples/modules",p/"source")
    entry=p/"source/main.hua";artifact=p/"模块 with spaces.huab"
    run("build",entry,"-o",artifact);original=artifact.read_bytes();run("build",entry,"-o",artifact)
    assert artifact.read_bytes()==original
    shutil.rmtree(p/"source")
    run("run",artifact,output="5\n4 6\n");run("bytecode",artifact)
    run("ast",artifact,code=2,error="Usage:");run("interpret",artifact,code=2,error="Usage:")
    entry=p/"bad.hua";entry.write_text('print("must not run")\nlet x=missing\n',encoding="utf-8")
    run("build",entry,"-o",artifact,code=1,error="E3001",silent=True)
    assert artifact.read_bytes()==original
    entry.write_text("fn f("+",".join("p"+str(i) for i in range(4097))+"){}\n",encoding="utf-8")
    run("build",entry,"-o",artifact,code=1,error="E6002",silent=True)
    assert artifact.read_bytes()==original
    entry.write_text("let "+"a"*4097+"=1\n",encoding="utf-8")
    run("build",entry,"-o",artifact,code=1,error="E6002",silent=True)
    assert artifact.read_bytes()==original
    def repack(payload):return original[:16]+struct.pack("<QII",len(payload),zlib.crc32(payload),0)+payload
    mutations=[original[:n] for n in (0,1,7,8,16,31,32,len(original)-1)]
    for offset,v in ((0,0),(8,99),(12,99),(16,0),(24,0),(28,1)):
        b=bytearray(original);b[offset]=v;mutations.append(bytes(b))
    mutations += [original+b"extra",original[:40]+bytes([original[40]^1])+original[41:],
                  repack(b"\xff\xff\xff\x7f"+original[36:]),
                  repack(struct.pack("<II",1,0xffffffff)+original[40:]),repack(original[32:]+b"trailing")]
    bad=p/"bad.huab"
    for b in mutations:bad.write_bytes(b);run("run",bad,code=1,error="E6002",silent=True)
    found=records(original)
    for opcode in (255,3,6,10,18):
        b=bytearray(original);b[found[0][0]]=opcode
        if opcode==6:struct.pack_into("<I",b,found[0][3],0xffffffff)
        bad.write_bytes(repack(b[32:]));run("run",bad,code=1,error="E6002",silent=True)
    seed=7
    for i in range(30):
        b=bytearray(original);seed=(seed*1664525+1013904223)&0xffffffff
        at=32+seed%(len(b)-32);b[at]^=1<<(i%8);bad.write_bytes(repack(b[32:]))
        result=subprocess.run([str(exe),"check",str(bad)],capture_output=True,encoding="utf-8",timeout=15)
        assert result.returncode in (0,1),result
    p=case()
    for name in ("main.hua","native_math.huam","native_math"+suffix,"wasm_math.wasm"):
        shutil.copy2(build/"extensions"/name,p/name)
    entry=p/"main.hua";artifact=entry.with_suffix(".huab");expected="42 42\nhello Hua true\n5 7\n"
    run("check",entry,output="OK\n")
    for engine in ("run","interpret"):run(engine,entry,output=expected)
    run("build",entry);run("run",artifact,output=expected)
    for name in ("main.hua","native_math.huam","wasm_math.wasm"):(p/name).unlink()
    run("check",artifact,output="OK\n");run("run",artifact,output=expected)
    (p/("native_math"+suffix)).unlink()
    run("check",artifact,output="OK\n");run("run",artifact,code=1,error="E7001",silent=True)
    for kind,code in ((1,"E7002"),(2,"E7003"),(3,"E7004"),(4,"E4003"),(5,"E7004")):
        p=case();shutil.copy2(build/"extensions"/(f"bad{kind}"+suffix),p/("bad"+suffix))
        (p/"bad.huam").write_text("HUA_NATIVE 1\npub fn value() int {}\n",encoding="utf-8")
        entry=p/"main.hua";entry.write_text("import bad\nprint(bad.value())\n",encoding="utf-8")
        run("check",entry,output="OK\n")
        for engine in ("run","interpret"):
            result=run(engine,entry,code=1,error=code,silent=True);assert "^" in result.stderr
    entry.write_text('import bad\nprint("must not run")\nmissing()\n',encoding="utf-8")
    run("run",entry,code=1,error="E3001",silent=True)
    p=case();shutil.copy2(build/"extensions"/("native_math"+suffix),p/("native_math"+suffix))
    shutil.copy2(root/"examples/extensions/native_math.huam",p/"native_math.huam")
    entry=p/"main.hua";entry.write_text('import native_math as n\nprint(n.greeting("中文"),n.greeting("a\\0b"))\nprint(n.add(9223372036854775807,1))\n',encoding="utf-8")
    run("run",entry,code=1,output="hello 中文 hello a\0b\n",error="native integer overflow")
    (p/"native_math.huam").write_text("HUA_NATIVE 2\n",encoding="utf-8");run("check",entry,code=1,error="E7002",silent=True)
    (p/"native_math.huam").write_text("HUA_NATIVE 1\npub fn value(x []int) int {}\n",encoding="utf-8");run("check",entry,code=1,error="E7003",silent=True)
    p=case();(p/"mod.wasm").write_bytes(compile_wat(runtime,"""(module
        (global $counter (mut i32) (i32.const 0))
        (func $start i32.const 10 global.set $counter) (start $start)
        (func (export "next") (result i32) global.get $counter i32.const 1 i32.add global.set $counter global.get $counter)
        (func (export "i64id") (param i64) (result i64) local.get 0)
        (func (export "f32id") (param f32) (result f32) local.get 0)
        (func (export "nop")))"""))
    entry=p/"main.hua";entry.write_text("import mod as a\nimport mod as b\nprint(a.next(),b.next(),a.i64id(9223372036854775807),a.f32id(1.5))\na.nop()\n",encoding="utf-8")
    for engine in ("run","interpret"):run(engine,entry,output="11 12 9223372036854775807 1.5\n")
    run("build",entry);(p/"mod.wasm").unlink();entry.unlink();run("run",p/"main.huab",output="11 12 9223372036854775807 1.5\n")
    def wasm_case(wat,body,code,error):
        p=case();(p/"mod.wasm").write_bytes(compile_wat(runtime,wat));entry=p/"main.hua";entry.write_text("import mod\n"+body,encoding="utf-8")
        for engine in ("run","interpret"):run(engine,entry,code=code,error=error,silent=True)
        return entry
    wasm_case('(module (func (export "id") (param i32) (result i32) local.get 0))',"mod.id(2147483648)\n",1,"E4002")
    wasm_case('(module (func (export "id") (param f32) (result f32) local.get 0))',"mod.id(1e300)\n",1,"E4002")
    wasm_case('(module (func (export "bad") (result f64) f64.const inf))',"mod.bad()\n",1,"E4002")
    wasm_case('(module (func (export "trap") unreachable))',"mod.trap()\n",1,"E7004")
    entry=wasm_case('(module (func (export "spin") (loop $loop br $loop)))',"mod.spin()\n",1,"E4099")
    run("build",entry);run("run",entry.with_suffix(".huab"),code=1,error="E4099",silent=True)
    wasm_case('(module (func $start (loop $loop br $loop)) (start $start))',"",1,"E4099")
    wasm_case('(module (memory 1025))',"",1,"E7003")
    wasm_case('(module (import "wasi_snapshot_preview1" "fd_write" (func)))',"",1,"imports")
    wasm_case('(module (func (export "many") (result i32 i32) i32.const 1 i32.const 2))',"",1,"one result")
    p=case();(p/"mod.wasm").write_bytes(b"not wasm");entry=p/"main.hua";entry.write_text("import mod\n",encoding="utf-8")
    run("check",entry,code=1,error="E7003",silent=True)
    (p/"mod.hua").write_text("pub fn value() int{return 7}\n",encoding="utf-8");entry.write_text("import mod\nprint(mod.value())\n",encoding="utf-8")
    run("run",entry,output="7\n")
    # Extension resolver and container output paths retain UTF-8 under Unicode directories.
    p=base/"中文目录";p.mkdir()
    shutil.copy2(build/"extensions"/("native_math"+suffix),p/("native_math"+suffix))
    shutil.copy2(root/"examples/extensions/native_math.huam",p/"native_math.huam")
    shutil.copy2(build/"extensions/wasm_math.wasm",p/"wasm_math.wasm")
    shutil.copy2(root/"examples/extensions/main.hua",p/"入口.hua")
    run("run",p/"入口.hua",output="42 42\nhello Hua true\n5 7\n")
    run("build",p/"入口.hua")
    run("run",p/"入口.huab",output="42 42\nhello Hua true\n5 7\n")
print(f"{count} persisted runtime fixtures; {checks} archive/Native/WASM CLI checks passed")
