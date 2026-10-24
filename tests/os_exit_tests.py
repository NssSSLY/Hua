# HUA-D038 step 5 group 3 (std.os exit and platform information) across three
# execution paths: exit codes, defer order, File flushing and the E4003/E4104 rules.
from pathlib import Path
import platform,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0

def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]

with tempfile.TemporaryDirectory(prefix='hua-os-exit-',dir=build) as temp:
    base=Path(temp);src=base/'source';src.mkdir();work=base/'work';work.mkdir();n=0
    def case(text,code,stdout=None,error=None,files=None):
        """Run text on the AST interpreter, the source VM and source-free HUAB."""
        global n,checks
        n+=1;p=src/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n')
        artifact=base/f'case{n}.huab'
        r=subprocess.run([str(exe),'build',str(p),'-o',str(artifact)],capture_output=True,timeout=60);assert r.returncode==0,r.stderr
        for command,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.rename(p.with_suffix('.moved'))
            for f in (files or {}):(work/f).unlink(missing_ok=True)
            proc=subprocess.Popen([str(exe),command,str(target)],cwd=work,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            out,err=proc.communicate(timeout=60);out=out.decode('utf-8').replace('\r\n','\n');err=err.decode('utf-8')
            assert proc.returncode==code,(n,command,proc.returncode,out,err)
            if stdout is not None:
                want=stdout(proc.pid) if callable(stdout) else stdout
                assert out==want,(n,command,out,want)
            if error:assert error in err,(n,command,err)
            if code!=1:assert 'error[' not in err,(n,command,err)
            for f,content in (files or {}).items():assert (work/f).read_text(encoding='utf-8')==content,(n,command,f)
            checks+=1
        if target==artifact:p.with_suffix('.moved').rename(p)
        return artifact

    # Exit code, nothing after exit runs, defers run innermost first, File data is flushed.
    artifact=case('''import std.os as os
import std.file as file
import std.bytes as bytes
fn bye(tag string) { print("defer", tag) }
fn inner() {
    defer bye("inner")
    print("before exit")
    os.exit(7)
    print("never")
}
fn main() {
    defer bye("main")
    let f = unwrap(file.open("exit_out.txt", "w"))
    unwrap(file.write(f, bytes.from_text("flushed")))
    inner()
    print("never2")
}
''',7,'before exit\ndefer inner\ndefer main\n',files={'exit_out.txt':'flushed'})
    assert header_format(artifact)==16,header_format(artifact)
    case('import std.os as os\nfn main() {\n    print("zero")\n    os.exit(0)\n}\n',0,'zero\n')
    case('import std.os as os\nfn main() { os.exit(125) }\n',125,'')
    # Exit from inside an L0-1 callback unwinds through the callback frames.
    case('import std.os as os\nimport std.alg as alg\nfn f(x int) int {\n    if x == 2 { os.exit(11) }\n    return x\n}\nfn main() {\n    defer print("main defer")\n    print(alg.map([1, 2, 3], f))\n}\n',11,'main defer\n')
    # Exit from a defer on a normal return.
    case('import std.os as os\nfn leave() { os.exit(4) }\nfn main() {\n    defer leave()\n    print("normal end")\n}\n',4,'normal end\n')
    # A diagnostic raised by a defer during exit cleanup replaces the exit code.
    case('import std.os as os\nfn boom() { print(1 // 0) }\nfn main() {\n    defer boom()\n    os.exit(9)\n}\n',1,error='E4004')
    # During diagnostic unwinding the first diagnostic stays; exit in a defer does not override it.
    case('import std.os as os\nfn leave() { os.exit(4) }\nfn main() {\n    defer leave()\n    print(1 // 0)\n}\n',1,error='E4004')
    # E4003 range, E4104 in child tasks / async main / during exit cleanup.
    case('import std.os as os\nfn main() { os.exit(126) }\n',1,error='E4003')
    case('import std.os as os\nfn main() { os.exit(-1) }\n',1,error='E4003')
    case('import std.os as os\nfn work() int {\n    os.exit(1)\n    return 0\n}\nfn main() {\n    let t = spawn work()\n    print(await t)\n}\n',1,error='E4104')
    case('import std.os as os\nasync fn main() {\n    os.exit(6)\n}\n',1,error='E4104')
    case('import std.os as os\nfn again() { os.exit(3) }\nfn main() {\n    defer again()\n    os.exit(5)\n}\n',1,error='os.exit during exit cleanup')
    # Platform information describes the running host.
    machine=platform.machine().lower();arch={'amd64':'x86_64','x86_64':'x86_64','arm64':'aarch64','aarch64':'aarch64'}.get(machine,machine)
    system={'Windows':'windows','Linux':'linux','Darwin':'macos'}.get(platform.system(),'other')
    case('import std.os as os\nfn main() {\n    print(os.platform(), os.arch(), os.pid(), os.cpu_count() >= 1)\n}\n',0,lambda pid:f'{system} {arch} {pid} true\n')
    old=base/'old.huab';q=src/'old.hua';q.write_text('import std.os as os\nfn main() { print(len(os.args())) }\n',encoding='utf-8')
    r=subprocess.run([str(exe),'build',str(q),'-o',str(old)],capture_output=True);assert r.returncode==0 and header_format(old)<16,header_format(old)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,15);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    r=subprocess.run([str(exe),'run',str(bad)],capture_output=True);assert r.returncode==1 and b'E6002' in r.stderr,r.stderr;checks+=1
    # Static checks.
    for body in ['os.exit("1")','os.exit(1.0)','print(os.platform(1))']:
        p=src/f'static{checks}.hua';p.write_text('import std.os as os\nfn main() {\n    '+body+'\n}\n',encoding='utf-8')
        r=subprocess.run([str(exe),'check',str(p)],capture_output=True);assert r.returncode==1 and b'E300' in r.stderr,(body,r.stderr);checks+=1
print(f'os exit/platform checks passed: {checks}')
