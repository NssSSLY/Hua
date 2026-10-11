"""A01 representative release matrix, SB1 closure guard and source-free HUAB proof."""
from pathlib import Path
import base64, hashlib, json, shutil, struct, subprocess, sys, tempfile
exe, probe, repo, build = (Path(s).resolve() for s in sys.argv[1:])
output_dir = build / "a01"
output_dir.mkdir(exist_ok=True)
(output_dir / "language-baseline-results.json").unlink(missing_ok=True)
catalog = json.loads((repo / "tests/data/language_baseline.json").read_text(encoding="utf-8"))
checks = 0
rows = []
def invoke(tool, action, path, case, work, expected=None, error=None, artifact=None):
    global checks
    work.mkdir(exist_ok=True)
    args = [str(tool), action, str(path)]
    if action in ("run", "interpret") and case.get("arguments"):
        args += ["--", *case["arguments"]]
    if action == "build" and artifact: args += ["-o", str(artifact)]
    result = subprocess.run(args, cwd=work, input=case.get("input", "").encode(), capture_output=True, timeout=25)
    out = result.stdout.decode("utf-8").replace("\r\n", "\n")
    err = result.stderr.decode("utf-8")
    assert result.returncode == (1 if error else 0), (case["id"], action, result.returncode, out, err)
    if expected is not None: assert out == expected, (case["id"], action, out, expected, err)
    if error: assert "error[" + error + "]" in err, (case["id"], action, err, error)
    assert not (work / "forbidden.txt").exists(), (case["id"], action, "side effect occurred")
    checks += 1
    return out

def guarded(text):
    # Keep imports at the beginning. Put a visible and filesystem effect before
    # statements/declarations so every rejection proves execution never began.
    lines=text.splitlines(keepends=True); at=0
    while at<len(lines) and (lines[at].lstrip().startswith("import ") or not lines[at].strip()): at+=1
    lines.insert(at, 'print("A01_SIDE_EFFECT")\nunwrap(a01_guard_fs.write_text("forbidden.txt","effect"))\n')
    return 'import std.fs as a01_guard_fs\n'+''.join(lines)

with tempfile.TemporaryDirectory(prefix="hua-a01-", dir=build) as tmp:
    root = Path(tmp)
    for case in catalog["features"]:
        directory=root/case["id"];directory.mkdir()
        source=directory/"main.hua";phase=case["phase"]
        if "fixture" in case:
            original=repo/case["fixture"];text=original.read_text(encoding="utf-8")
            stdout=original.with_suffix(".stdout")
            expected=stdout.read_text(encoding="utf-8") if stdout.exists() else ""
        else:text=case["source"];expected=case.get("output","")
        for name,data in case.get("modules",{}).items():
            dep=directory/name;dep.parent.mkdir(parents=True,exist_ok=True);dep.write_text(data,encoding="utf-8")
        rejected=phase in ("parse","static")
        source.write_text(guarded(text) if rejected else text,encoding="utf-8")
        parser_error=case.get("error") if phase=="parse" else None
        invoke(exe,"ast",source,case,directory/"parse",error=parser_error)
        if rejected:
            for action in ("check","interpret","run","build"):
                invoke(exe,action,source,case,directory/action,expected="",error=case["error"])
            assert not source.with_suffix(".huab").exists()
            rows.append({"id":case["id"],"parse":"reject" if parser_error else "pass","static":"reject","AST":"not executed","VM":"not executed","HUAB":"not emitted","error":case["error"]})
            continue
        invoke(exe,"check",source,case,directory/"check",expected="OK\n")
        runtime_error=case.get("error") if phase=="runtime" else None
        for action in ("interpret","run"):
            invoke(exe,action,source,case,directory/action,expected=expected,error=runtime_error)
        artifact=source.with_suffix(".huab")
        invoke(exe,"build",source,case,directory/"build")
        version,abi=struct.unpack_from("<II",artifact.read_bytes(),8)
        assert abi==1 and version in (6,7,8),(case["id"],version,abi)
        checks+=1
        if phase=="runtime" and case["sb1"]:
            invoke(probe,"check",source,case,directory/"subset-check",expected="SB1 OK\n")
        if phase=="execute":
            if case["sb1"]:
                invoke(probe,"check",source,case,directory/"subset-check",expected="SB1 OK\n")
                invoke(probe,"run",source,case,directory/"subset-run",expected=expected)
            else:
                source.write_text(guarded(text),encoding="utf-8")
                denied=directory/"denied.huab"
                for action in ("check","interpret","run","build"):
                    invoke(probe,action,source,case,directory/("subset-"+action),expected="",error="E8101",artifact=denied)
                assert not denied.exists()
        # Remove the entire local source closure, not just the entry.
        for p in directory.rglob("*.hua"):p.unlink()
        invoke(exe,"check",artifact,case,directory/"huab-check",expected="OK\n")
        invoke(exe,"run",artifact,case,directory/"huab-run",expected=expected,error=runtime_error)
        rows.append({"id":case["id"],"parse":"pass","static":"pass","AST":"runtime error" if runtime_error else "pass","VM":"runtime error" if runtime_error else "pass","HUAB":"runtime error" if runtime_error else "pass","version":version,"SB1":"accept" if case["sb1"] else "exclude","error":runtime_error})
    # Real HUAB6 artifacts captured before A01: valid checksums and source spans,
    # with visible/file effects before unsupported operations. Reject at read time.
    legacy=json.loads((repo/'tests/data/a01_unsupported_archives.json').read_text(encoding='utf-8'))
    for old in legacy:
        directory=root/old['id'];directory.mkdir();p=directory/'old.huab';raw=base64.b64decode(old['base64'],validate=True)
        assert hashlib.sha256(raw).hexdigest()==old['sha256'];p.write_bytes(raw)
        for action in ('check','bytecode','run'):
            invoke(exe,action,p,{'id':old['id']},directory/action,expected='',error='E6002')
    # Excluded declarations in an otherwise valid transitive local dependency.
    for tag, contents in {
        "nested-async":{'lib.hua':'import dep\npub fn value() int{return 1}\n','dep.hua':'async fn f() int{return 2}\n'},
        "nested-generic":{'lib.hua':'import dep\npub fn value() int{return 1}\n','dep.hua':'fn f<T>(x T) T{return x}\n'},
        "native":{'lib.huam':'HUA_NATIVE 1\npub fn value() int {}\n','lib.dll':'','lib.so':'','lib.dylib':''},
        "reserved-prefix":{'lib.hua':'pub fn __private() int{return 1}\n'},
        "forbidden-type":{'lib.hua':'pub fn value(x Result<int,int>) int{return 1}\n'},
    }.items():
        directory=root/tag;directory.mkdir();source=directory/'main.hua'
        source.write_text(guarded('import lib\nprint("entry")\n'),encoding='utf-8')
        for name,data in contents.items():(directory/name).write_text(data,encoding='utf-8')
        case={'id':tag}
        for action in ('check','interpret','run','build'):
            invoke(probe,action,source,case,directory/action,expected='',error='E8101')
        assert not source.with_suffix('.huab').exists()
    # A new local module named "stdlib" is not the reserved std namespace.
    directory=root/'stdlib-name';directory.mkdir()
    (directory/'stdlib.hua').write_text('pub fn value() int{return 7}\n',encoding='utf-8')
    source=directory/'main.hua';source.write_text('import stdlib\nprint(stdlib.value())\n',encoding='utf-8')
    invoke(probe,'run',source,{'id':'stdlib-name'},directory/'run',expected='7\n')
    # Current Hua lexer is genuine SB1 source, including its local closure.
    entry=repo/'selfhost/main.hua';sample=root/'sample.hua';sample.write_text('print(1)\n',encoding='utf-8')
    case={'id':'selfhost','arguments':[str(sample)]}
    invoke(probe,'check',entry,case,root/'selfhost-check',expected='SB1 OK\n')
    reference=invoke(exe,'interpret',entry,case,root/'selfhost-reference')
    for action in ('interpret','run'):invoke(probe,action,entry,case,root/('selfhost-'+action),expected=reference)
    archived=root/'selfhost.huab';invoke(probe,'build',entry,case,root/'selfhost-build',artifact=archived)
    isolated=root/'isolated';isolated.mkdir();shutil.copy2(archived,isolated/'lexer.huab')
    # Source-free copy cannot resolve selfhost modules from this directory.
    invoke(exe,'run',isolated/'lexer.huab',case,root/'selfhost-huab',expected=reference)
report={'catalog':catalog['version'],'subset':'SB1','result':'passed','cases':len(rows),'checks':checks,'features':rows,'transitive_exclusions':5,'legacy_unsupported_archives':len(legacy),'selfhost_lexer':'check/AST/VM/HUAB passed','platform':sys.platform,'executable':str(exe),'probe':str(probe)}
(output_dir/'language-baseline-results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(f"A01: {len(rows)} capability cases, {checks} checks; SB1 closure guard and selfhost lexer passed")
