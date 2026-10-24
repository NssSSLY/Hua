# HUA-D038 step 5 group 4 (std.fs walk/copy_file/copy_dir/remove_all/canonical)
# across three execution paths. Each path runs in a fresh working directory.
from pathlib import Path
import os,shutil,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0
def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]

PROGRAM='''import std.fs as fs
fn main() {
    unwrap(fs.mkdir_all("t/b/deep"))
    unwrap(fs.mkdir_all("t/a"))
    unwrap(fs.mkdir_all("t/empty"))
    unwrap(fs.write_text("t/a/x.txt", "x"))
    unwrap(fs.write_text("t/b/deep/y.txt", "yy"))
    unwrap(fs.write_text("t/中文.txt", "z"))
    unwrap(fs.write_text("t/Z.txt", "z"))
    print(unwrap(fs.walk("t")))
    print(fs.walk("missing"), fs.walk("t/Z.txt"))
    print(unwrap(fs.copy_file("t/a/x.txt", "t/a/x2.txt")), unwrap(fs.read_text("t/a/x2.txt")))
    print(is_err(fs.copy_file("t/a/x.txt", "t/a/x2.txt")), is_err(fs.copy_file("t/a", "t/a3")), fs.copy_file("t/a/x.txt", "t/a/x.txt"), is_err(fs.copy_file("t/nope", "t/n2")))
    print(unwrap(fs.copy_dir("t", "t2")), unwrap(fs.walk("t2")) == unwrap(fs.walk("t")), unwrap(fs.read_text("t2/b/deep/y.txt")))
    print(is_err(fs.copy_dir("t", "t2")), fs.copy_dir("t", "t/inner"), is_err(fs.copy_dir("t/Z.txt", "t3")))
    print(unwrap(fs.canonical("t/a/../b")) == unwrap(fs.canonical("t/b")), is_err(fs.canonical("nope")))
    print(unwrap(fs.remove_all("t2")), unwrap(fs.remove_all("t2")), fs.remove_all("."), fs.remove_all(".."))
    print(unwrap(fs.remove_all("t")), unwrap(fs.exists("t")))
}
'''
EXPECTED='''[Z.txt, a/, a/x.txt, b/, b/deep/, b/deep/y.txt, empty/, 中文.txt]
err(FS_NOT_FOUND: walk root does not exist) err(FS_TYPE: walk root must be a directory (links are not followed))
true x
true true err(FS_SAME_PATH: source and destination are identical) true
5 true yy
true err(FS_TYPE: copy_dir target is inside the source) true
true true
10 0 err(FS_TYPE: remove_all refuses the startup directory and its ancestors) err(FS_TYPE: remove_all refuses the startup directory and its ancestors)
10 false
'''

with tempfile.TemporaryDirectory(prefix='hua-fs-walk-',dir=build) as temp:
    base=Path(temp);src=base/'source';src.mkdir();n=0
    def paths(text):
        global n
        n+=1;p=src/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');artifact=base/f'case{n}.huab'
        r=subprocess.run([str(exe),'build',str(p),'-o',str(artifact)],capture_output=True,timeout=60);assert r.returncode==0,r.stderr
        hidden=p.with_suffix('.moved')
        return p,artifact,hidden
    def run_all(text,want=None,code=0,error=None,setup=None):
        global checks
        p,artifact,hidden=paths(text)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:
            work=base/f'work{n}_{command}_{target.suffix[1:]}';work.mkdir()
            if setup:setup(work)
            if target==artifact:p.rename(hidden)
            r=subprocess.run([str(exe),command,str(target)],cwd=work,capture_output=True,timeout=120)
            if target==artifact:hidden.rename(p)
            out=r.stdout.decode('utf-8').replace('\r\n','\n');err=r.stderr.decode('utf-8')
            assert r.returncode==code,(n,command,r.returncode,out,err)
            if want is not None:assert out==want,(n,command,out,want)
            if error:assert error in err,(n,command,err)
            checks+=1
        return artifact
    artifact=run_all(PROGRAM,EXPECTED)
    assert header_format(artifact)==17,header_format(artifact)
    # Depth limit: 257 nested directories.
    long_prefix='\\\\?\\' if os.name=='nt' else ''  # 257 levels exceed MAX_PATH
    long_dirs=[]
    def deep(work):
        long_dirs.append(work/'deep')
        d=str(work/'deep')
        for i in range(257):d+=os.sep+'d'
        os.makedirs(long_prefix+d)
    run_all('import std.fs as fs\nfn main() { print(fs.walk("deep")) }\n','err(FS_LIMIT: directory depth exceeds 256)\n',setup=deep)
    for d in long_dirs:shutil.rmtree(long_prefix+str(d))
    # Links are listed but never followed; copy_dir refuses them (only when this host lets us create one).
    def linked(work):
        (work/'t'/'real').mkdir(parents=True);(work/'t'/'real'/'f.txt').write_text('f')
        os.symlink(work/'t'/'real',work/'t'/'link',target_is_directory=True)
    probe=base/'probe';probe.mkdir()
    try:os.symlink(probe,base/'probe_link',target_is_directory=True);can_link=True
    except OSError:can_link=False
    if can_link:
        run_all('import std.fs as fs\nfn main() {\n    print(unwrap(fs.walk("t")))\n    print(is_err(fs.copy_dir("t", "u")))\n    print(unwrap(fs.remove_all("t")), unwrap(fs.exists("t")))\n}\n','[link, real/, real/f.txt]\ntrue\n4 false\n',setup=linked)
    else:print('symlink cases skipped: this host does not allow creating symbolic links')
    # walk from a Task (blocking adaptation).
    run_all('import std.fs as fs\nfn scan() int { return len(unwrap(fs.walk("w"))) }\nfn main() {\n    unwrap(fs.mkdir_all("w/x"))\n    unwrap(fs.write_text("w/x/a.txt", "a"))\n    let t = spawn scan()\n    print(await t)\n    print(unwrap(fs.remove_all("w")))\n}\n','2\n3\n')
    # Path validation follows S1a.
    run_all('import std.fs as fs\nfn main() { print(fs.walk("")) }\n','err(PATH_INVALID: non-NUL UTF-8 path required)\n')
    old=base/'old.huab';q=src/'old.hua';q.write_text('import std.fs as fs\nfn main() { print(fs.exists("x")) }\n',encoding='utf-8')
    r=subprocess.run([str(exe),'build',str(q),'-o',str(old)],capture_output=True);assert r.returncode==0 and header_format(old)<17,header_format(old);checks+=1
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,16);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    r=subprocess.run([str(exe),'run',str(bad)],capture_output=True);assert r.returncode==1 and b'E6002' in r.stderr,r.stderr;checks+=1
    for body in ['print(fs.walk(1))','print(fs.copy_file("a"))','print(fs.remove_all(true))']:
        p=src/f'static{checks}.hua';p.write_text('import std.fs as fs\nfn main() {\n    '+body+'\n}\n',encoding='utf-8')
        r=subprocess.run([str(exe),'check',str(p)],capture_output=True);assert r.returncode==1 and b'E300' in r.stderr,(body,r.stderr);checks+=1
print(f'fs walk/copy checks passed: {checks}')
