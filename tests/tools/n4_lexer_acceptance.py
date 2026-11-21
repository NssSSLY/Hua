"""Verify the N4.2 native container and Result path with the real Hua lexer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

from n4_records_acceptance import verify_records
from n4_scalar_acceptance import (
    DRIVER, ROOT, SUPERVISOR_SECONDS, bootstrap, diagnostic, digest, run, save, verify_programs,
)


LEXER_ENTRY = """import lexer
import std.fs as fs
import std.os as os

fn main() Result<void> {
    let args=os.args()
    let source=fs.read_text(args[0])?
    let tokens=lexer.scan_bounded(source,int(args[1]))?
    for token in tokens {
        print(token.kind,token.line,token.column,token.start,token.end,len(token.text),token.text)
    }
    return ok()
}
"""

CONTAINERS = """import std.list as list
import std.bytes as bytes
struct Cell { value int }
fn mutate(items mut []Cell) { items[0].value=9 }
var data=[2,3,4]
fn index() int { print("index")
    return 0 }
fn replace() int { print("rhs")
    data=[90,91]
    return 5 }
fn main() Result<void> {
    var cells=[Cell{value:1},Cell{value:2}]
    var view=cells[:1]
    mutate(view)
    print(cells[0].value,cells[1].value)
    var alias=data[0:2]
    data[index()]+=replace()
    print(alias[0],data[0])
    var sum=0
    for i,item in alias { sum+=i+item }
    for i in 4..=0 by -2 {
        if i==2 { continue }
        sum+=i
    }
    for i in 9223372036854775807..=9223372036854775807 { print(i)
        break }
    print(sum)
    var items=list.from_slice(cells)
    let frozen=list.snapshot(items)
    list.append(items,Cell{value:11})?
    let last=list.pop(items)?
    list.set(items,0,Cell{value:13})?
    print(len(frozen),list.len(items),last.value,list.get(items,1)?.value,frozen[0].value,list.get(items,0)?.value)
    var words=map[string]int{"b":2,"a":1}
    var visits=0
    for key,value in words {
        visits+=value
        words[key]+=10
        words["new"]=20
    }
    print(visits,len(words),has(words,"a"),unwrap(words["a"]),unwrap_or(words["none"],99))
    let raw=bytes.from_text("中\\0")
    print(bytes.len(raw),bytes.at(raw,3)?,len(bytes.to_ints(raw)?))
    return ok()
}
"""

PROPAGATION = """fn step(value int) Result<int> {
    print(value)
    if value==2 { return err("stopped") }
    return ok(value)
}
fn join(a int,b int,c int) int { return a+b+c }
fn main() Result<void> {
    if false && step(9)?>0 { print("short-circuit failed") }
    print(join(step(1)?,step(2)?,step(3)?))
    print("unexpected continuation")
    return ok()
}
"""

OPTIONAL = """fn choose(yes bool) int? {
    if yes { return 3 }
}
fn main() {
    let present=choose(true)
    let absent=choose(false)
    print(is_some(present),is_none(absent),unwrap_or(absent,8))
    if present!=nil { print(present+1) }
}
"""

TEXT_SLICE = """import std.strings as strings
fn main() {
    let text="中abc"
    print(unwrap(strings.slice(text,0,3)))
    print(unwrap_err(strings.slice(text,1,3)))
    print(unwrap_err(strings.slice(text,-1,3)))
}
"""

CASES = {
    "containers": (CONTAINERS, "9 2\nindex\nrhs\n7 90\n9223372036854775807\n15\n2 2 11 2 9 13\n3 3 true 11 99\n4 0 4\n", None),
    "propagation": (PROPAGATION, "1\n2\n", "E0003"),
    "optional": (OPTIONAL, "true true 8\n4\n", None),
    "text_slice": (TEXT_SLICE, "中\nstring slice cuts a UTF-8 character\nstring slice bounds are invalid\n", None),
    "slice_bounds": ('fn main() { var data=[1]\ndata[2]=99\n}\n', "", "E4006"),
    "missing_map_key": ('fn main() { var m=map[string]int{}\nm["missing"]+=1\n}\n', "", "E4006"),
    "zero_step": ('fn main() { for i in 0..5 by 0 { print(i) } }\n', "", "E4005"),
}


def compile_native(host, generator, entry, work):
    product = work / "program.exe"
    common = [sys.executable, DRIVER, entry, "--hua", host, "--generator", generator]
    result = run(common + ["-o", product], work, SUPERVISOR_SECONDS + 5)
    receipt = json.loads(result["stdout"])
    translation = product.with_suffix(".cpp")
    run(common + ["--emit-cpp", "-o", translation], work, SUPERVISOR_SECONDS + 5)
    assert digest(translation) == receipt["cpp_sha256"]
    return product, receipt


def compare(host, entry, product, cwd, args, error, archive=None):
    expected = int(error is not None)
    paths = {
        action: run([host, action, entry, "--execution-profile", "compiler", "--", *args],
                    cwd, SUPERVISOR_SECONDS, expected=expected)
        for action in ("interpret", "run")
    }
    if archive:
        paths["huab"] = run([host, "run", archive, "--execution-profile", "compiler", "--", *args],
                            cwd, SUPERVISOR_SECONDS, expected=expected)
    paths["native"] = run([product, *args], cwd, SUPERVISOR_SECONDS, expected=expected)
    baseline = paths["run"]
    for outcome in paths.values():
        assert outcome["stdout"] == baseline["stdout"], paths
        if error:
            assert diagnostic(outcome["stderr"]) == diagnostic(baseline["stderr"]), paths
            assert diagnostic(outcome["stderr"])[0] == error, paths
            assert outcome["stderr"].splitlines()[0] == baseline["stderr"].splitlines()[0], paths
        else:
            assert not outcome["stderr"], outcome
    return paths


def verify_language(host, generator, work):
    report = {}
    for name, (source, expected, error) in CASES.items():
        case = work / name
        case.mkdir()
        entry = case / "main.hua"
        entry.write_text(source, encoding="utf-8", newline="\n")
        product, receipt = compile_native(host, generator, entry, case)
        results = compare(host, entry, product, case, [], error)
        assert results["native"]["stdout"] == expected, results
        if name == "propagation":
            for result in results.values():
                assert "main returned Err: stopped" in result["stderr"], result
                assert "Result propagated here" in result["stderr"], result
        report[name] = dict(receipt=receipt, paths=results)
        print(name, "three paths agree", flush=True)
    return report


def verify_lexer(host, generator, work, seed=None):
    source = work / "lexer-source"
    source.mkdir()
    entry = source / "main.hua"
    entry.write_text(LEXER_ENTRY, encoding="utf-8", newline="\n")
    shutil.copyfile(ROOT / "selfhost/lexer.hua", source / "lexer.hua")
    product, receipt = compile_native(host, generator, entry, work)
    archive = work / "lexer-archive" / "probe.huab" if seed else None
    archived = bootstrap(host, seed, entry, archive) if archive else None
    inputs = {
        "compiler_lexer": ((ROOT / "selfhost/lexer.hua").read_bytes(), 10000, ""),
        "compiler_lowering": ((ROOT / "selfhost/lowering.hua").read_bytes(), 24000, ""),
        "escaped_comments": ('\ufeff#* outer #** inner **# *#\r\nlet x="零\\0\\\\\\n"\r\n'
                             'var y=0x1f+0b11+0o10+1_000+2.3e-2\r\n'.encode("utf-8"), 300, ""),
        "character": ("中".encode("utf-8"), 10, "E1001"),
        "number": (b"0x_\n", 10, "E1002"),
        "string": (b'"text\n', 10, "E1003"),
        "escape": (b'"\\q"\n', 10, "E1004"),
        "comment": (b"#* unfinished\n", 10, "E1005"),
        "token_budget": (b"let a=1\n", 2, "E8202"),
        "invalid_budget": (b"", 0, "E8202"),
    }
    report = {}
    for name, (data, limit, code) in inputs.items():
        path = work / f"{name}.hua"
        path.write_bytes(data)
        args = [path, str(limit)]
        results = compare(host, entry, product, work, args, "E0003" if code else None, archive)
        if code:
            for result in results.values():
                assert f"main returned Err: {code}@" in result["stderr"], result
        else:
            assert "\nend " in results["native"]["stdout"], results["native"]["stdout"][-400:]
        stdout = results["native"]["stdout"].encode("utf-8")
        report[name] = dict(input_sha256=digest(path), stdout_sha256=hashlib.sha256(stdout).hexdigest(),
                            stdout_bytes=len(stdout), error=code,
                            paths={key: {k: v for k, v in row.items() if k!="stdout"}
                                   for key, row in results.items()})
        print(name, "lexer paths agree", flush=True)
    detached=work / "lexer-source-moved"
    source.rename(detached)
    expected=report["escaped_comments"]["stdout_sha256"]
    replay=run([product, work/"escaped_comments.hua", "300"], work)
    assert hashlib.sha256(replay["stdout"].encode("utf-8")).hexdigest()==expected
    if archive:
        replay_huab = run([host, "run", archive, "--", work/"escaped_comments.hua", "300"], work)
        assert replay_huab["stdout"] == replay["stdout"]
    return dict(lexer_sha256=digest(detached/"lexer.hua"), build=receipt, archive=archived,
                inputs=report, detached=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("generator", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--seed", type=Path)
    parser.add_argument("--regression", action="store_true")
    options = parser.parse_args()
    host, generator, work = (p.resolve() for p in (options.host, options.generator, options.out))
    work.mkdir(parents=True, exist_ok=False)
    os.environ["PATH"] = str(host.parent)+os.pathsep+os.environ["PATH"]
    seed = options.seed.resolve() if options.seed else None
    report = {"generator_sha256": digest(generator), "lexer": verify_lexer(host,generator,work,seed)}
    report["language"] = verify_language(host,generator,work)
    if options.regression:
        report["records"] = verify_records(host,generator,work)
        report["scalar"] = verify_programs(host,generator,work)
    save(work / "report.json", report)


if __name__=="__main__":
    main()
