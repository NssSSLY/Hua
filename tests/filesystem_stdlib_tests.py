"""S1a path/file contracts across interpreter, VM and source-free HUAB."""
from pathlib import Path
import json, os, random, struct, subprocess, sys, tempfile
exe, root, build = (Path(x).resolve() for x in sys.argv[1:4])
legacy = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else None
checks = fixtures = 0
covered = set()
def literal(s): return json.dumps(str(s), ensure_ascii=False)
def run(command, path, code=0, error=None, cwd=None, binary=exe):
    global checks
    p=subprocess.run([str(binary),command,str(path)],cwd=cwd or build,capture_output=True,timeout=25)
    out=p.stdout.decode("utf-8").replace("\r\n","\n");err=p.stderr.decode("utf-8")
    assert p.returncode==code,(command,path,p.returncode,out,err)
    if error:assert error in err,(command,path,err,error)
    checks+=1;return out

with tempfile.TemporaryDirectory(prefix="hua-s1a-",dir=build) as tmp:
    base=Path(tmp)
    def triple(body,want,prepare=None,verify=None):
        global fixtures
        fixtures+=1;folder=base/str(fixtures);folder.mkdir()
        source=folder/"main.hua"
        source.write_text("import std.path as p\nimport std.fs as f\nimport std.json as j\n"+body,encoding="utf-8")
        assert run("check",source)=="OK\n"
        run("build",source);artifact=source.with_suffix(".huab")
        assert struct.unpack_from("<II",artifact.read_bytes(),8)==(6,1)
        outputs=[]
        for engine in ("interpret","run","huab"):
            if prepare:prepare(folder)
            if engine=="huab":source.unlink()
            out=run("run" if engine=="huab" else engine,artifact if engine=="huab" else source,cwd=folder)
            if callable(want):want(out,folder)
            else:assert out==want,(body,out,want)
            outputs.append(out)
            if verify:verify(folder)
        assert run("check",artifact)=="OK\n"
        return artifact
    def expr(op,args,want):
        covered.add("path."+op)
        code="print(unwrap(j.encode(unwrap(p."+op+"("+args+")))))\n"
        return triple(code,literal(want)+"\n")
    expr("join",'["folder","中文 空格","..","file.txt"]',"folder/file.txt")
    expr("join",'["a","../b"]',"b")
    expr("join",'["",""]',".")
    triple('let empty []string=[]\nprint(unwrap(p.join(empty)))\n',".\n")
    for text,want in [("","."),("a//b/./../c/","a/c"),("../a/../../b","../../b"),("a/..","."),(".hidden",".hidden")]:
        expr("normalize",literal(text),want)
    for text,want in [("a/b/","b"),("","."),("a/..",".")]:expr("basename",literal(text),want)
    for text,want in [("a/b/","a"),("a","."),("../a","..")]:expr("dirname",literal(text),want)
    for text,want in [("a.tar.gz",".gz"),(".profile",""),("a.","."),("..",""),("a","")]:expr("ext",literal(text),want)
    for target,start,want in [("a/b","a","b"),("a","a/b",".."),("a","a","."),("../a","../b","../a")]:
        expr("relative",literal(target)+","+literal(start),want)
    rng=random.Random(811)
    for _ in range(12):
        parts=[rng.choice(["a","b","..",".","中文",""]) for _ in range(12)]
        # Independent stack reference on relative paths, avoiding OS filesystem traversal.
        stack=[]
        for item in parts:
            if item in ("","."):continue
            if item==".." and stack and stack[-1]!="..":stack.pop()
            else:stack.append(item)
        expr("normalize",literal("./"+"/".join(parts)), "/".join(stack) or ".")
    if os.name=="nt":
        expr("normalize",literal(r"C:\a\..\中文 空格"),"C:/中文 空格")
        expr("join",json.dumps(["ignored",r"D:\x","y"]),"D:/x/y")
        expr("normalize",literal(r"\\server\share\a\..\..\b"),"//server/share/b")
        expr("relative",literal(r"C:\A\b")+","+literal(r"c:\a"),"b")
        for text in ["C:relative","/root-relative",r"\\server",r"\\?\C:\a"]:
            triple("print(is_err(p.normalize("+literal(text)+")))\n","true\n")
        triple('print(is_err(p.relative("C:/a","D:/a")))\n',"true\n")
        triple('print(is_err(f.stat("NUL")))\n',"true\n")
    else:
        expr("normalize","\"/a/../../b\"","/b")
        expr("join",'["a","/b","c"]',"/b/c")
    triple('print(is_err(p.normalize("a\\0b")),is_err(p.relative("a","../b")))\n',"true true\n")
    # Static rejection is distinct from Result failure.
    for body in ['p.join([1,2])','p.normalize(1)','f.stat(true)','f.rename("a")','f.temp_file("a",2)']:
        fixtures+=1;source=base/f"invalid{fixtures}.hua"
        source.write_text("import std.path as p\nimport std.fs as f\n"+body+"\n",encoding="utf-8")
        for engine in ("check","interpret","run","build"):run(engine,source,1,"E300")
    covered.add("fs.stat")
    def stat_check(out,folder):
        rows=[json.loads(line) for line in out.splitlines()]
        assert rows[0]["kind"]=="file" and rows[0]["size_bytes"]==len("中文\n".encode())
        assert isinstance(rows[0]["modified_ms"],int)
        assert Path(rows[0]["path"]).is_absolute()
        assert rows[1]["kind"]=="directory" and rows[1]["size_bytes"] is None
        assert rows[2] is True
    triple('print(unwrap(j.encode(unwrap(f.stat("中文 空格.txt")))))\nprint(unwrap(j.encode(unwrap(f.stat(".")))))\nprint(is_err(f.stat("missing")))\n',stat_check,
           lambda d:(d/"中文 空格.txt").write_text("中文\n",encoding="utf-8",newline="\n"))
    covered.update(["fs.rename","fs.replace"])
    def prepare(d):
        for name in ["old","renamed","target","replacement","existing"]:
            path=d/name
            if path.exists():path.unlink()
        (d/"old").write_bytes(b"old");(d/"target").write_bytes(b"old-target")
        (d/"replacement").write_bytes(b"new-target");(d/"existing").write_bytes(b"keep")
    body='''print(unwrap(f.rename("old","renamed")))
print(is_err(f.rename("renamed","existing")))
print(unwrap(f.replace("replacement","target")))
print(is_err(f.replace("missing","target")))
print(unwrap(f.read_text("target")),unwrap(f.read_text("renamed")),unwrap(f.read_text("existing")))
'''
    def verify(d):
        assert (d/"target").read_bytes()==b"new-target"
        assert (d/"renamed").read_bytes()==b"old" and (d/"existing").read_bytes()==b"keep"
        assert not (d/"old").exists() and not (d/"replacement").exists()
    triple(body,"true\ntrue\ntrue\ntrue\nnew-target old keep\n",prepare,verify)
    # Replacing an absent target, directory rename, same path and type errors.
    def prepare2(d):
        if (d/"moved").exists():(d/"moved").rmdir()
        (d/"directory").mkdir(exist_ok=True)
        if (d/"fresh").exists():(d/"fresh").unlink()
        (d/"file").write_bytes(b"value")
    triple('print(unwrap(f.rename("directory","moved")))\nprint(unwrap(f.replace("file","fresh")))\nprint(is_err(f.replace("fresh","moved")),is_err(f.rename("fresh","fresh")))\nprint(unwrap(f.read_text("fresh")))\n',
           "true\ntrue\ntrue true\nvalue\n",prepare2)
    # temp_file creates a closed, ordinary file, returns an absolute path; user owns cleanup.
    covered.add("fs.temp_file")
    def temp_check(out,d):
        lines=out.splitlines();path=Path(json.loads(lines[0]))
        assert path.is_absolute() and path.parent==d
        assert path.name.startswith("中文-")
        assert lines[1:]==["true","0","true","true true"]
        assert not path.exists()
    triple('let path=unwrap(f.temp_file(".","中文-"))\nprint(unwrap(j.encode(path)))\nprint(unwrap(f.exists(path)))\nlet info=unwrap(f.stat(path))\nprint(unwrap(j.as_int(unwrap(j.get(info,"size_bytes")))))\nprint(unwrap(f.remove(path)))\nprint(is_err(f.temp_file(".","../bad")),is_err(f.temp_file("missing","hua-")))\n',temp_check)
    triple('let path=unwrap(f.temp_file("",""))\nprint(unwrap(f.exists(path)),unwrap(f.remove(path)))\n',"true true\n")
    triple('async fn read_kind(path string) string {return unwrap(j.as_string(unwrap(j.get(unwrap(f.stat(path)),"kind"))))}\nfn main(){let t=read_kind(".")\nprint(await t)}\n',"directory\n")
    symlink_verified=False
    probe=base/"link-probe"
    try:
        os.symlink("missing",probe)
        probe.unlink()
    except (OSError,NotImplementedError):
        pass
    else:
        def link_prepare(d):
            if (d/"link").is_symlink():(d/"link").unlink()
            os.symlink("missing",d/"link")
        triple('let info=unwrap(f.stat("link"))\nprint(unwrap(j.as_string(unwrap(j.get(info,"kind")))),j.is_null(unwrap(j.get(info,"size_bytes"))),j.is_null(unwrap(j.get(info,"modified_ms"))))\nprint(is_err(f.replace("link","destination")))\n',"symlink true true\ntrue\n",link_prepare)
        symlink_verified=True
    def check_error(out):
        assert json.loads(out).startswith("FS_NOT_FOUND:")
    triple('let error=unwrap_err(f.rename("missing","other"))\nprint(unwrap(j.encode(error)))\n',lambda out,d: check_error(out))
    # Returned path resolves from startup cwd even when the entry file is elsewhere.
    source=base/"cwd.hua";source.write_text('import std.fs as f\nprint(unwrap(f.read_text("data")))\n',encoding="utf-8")
    place=base/"cwd";place.mkdir();(place/"data").write_bytes(b"startup")
    assert run("run",source,cwd=place)=="startup\n"
    assert covered=={"path."+x for x in ["join","normalize","basename","dirname","ext","relative"]}|{"fs."+x for x in ["stat","rename","replace","temp_file"]}
    example=(root/"examples/local_paths.hua").read_text(encoding="utf-8")
    example="\n".join(line for line in example.splitlines() if not line.startswith("import "))
    triple(example,"b demo.txt .txt\nfile 8\n")
    # New references are rejected by the actual old executable; legacy HUAB still loads.
    compatibility=None
    if legacy and legacy.exists():
        artifact=expr("normalize",'"a/../b"',"b")
        run("check",artifact,1,"E6002",binary=legacy)
        old=base/"old.hua";old.write_text('print("legacy")\n',encoding="utf-8")
        run("build",old,binary=legacy);old.unlink()
        assert run("run",old.with_suffix(".huab"))=="legacy\n"
        compatibility=True
    report={"cases":fixtures,"checks":checks,"covered":sorted(covered),"legacyCompatibility":compatibility,"symlinkVerified":symlink_verified,"platform":sys.platform,"passed":True}
    (build/"filesystem_stdlib_verification.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(report,ensure_ascii=False))
