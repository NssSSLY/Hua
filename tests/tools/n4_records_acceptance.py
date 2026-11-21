"""Compare N4.2A records, strings and initialization with AST and VM."""
import argparse
import json
import os
from pathlib import Path
import sys

from n4_scalar_acceptance import DRIVER, SUPERVISOR_SECONDS, diagnostic, digest, run, save, verify_programs


CASES = {
    "diamond": ({
        "main.hua": """import left
import right
var token=left.read()
print("root",token.text)
fn main() {
    var copied=token
    token.text=right.read().text
    print(copied.text,token.text,left.read().text)
}
""",
        "left.hua": """import shared
print("left")
pub fn read() shared.Token { return shared.next() }
""",
        "right.hua": """import shared
print("right")
pub fn read() shared.Token { return shared.next() }
""",
        "shared.hua": """pub struct Token { text string
    span Span }
struct Span { count int }
print("shared")
var count=0
pub fn next() Token {
    count=count+1
    return Token{span:Span{count:count},text:"词"+str(count)}
}
""",
    }, "shared\nleft\nright\nroot 词1\n词1 词2 词3\n", None),
    "fields": ({
        "main.hua": """struct Outer { nested Inner }
struct Inner { text string
    length int }
fn observed(text string) string { print(text)
    return text
}
fn sample() Outer {
    return Outer{nested:Inner{length:len(observed("len")),text:observed("text")}}
}
var global=sample()
fn replace() string {
    global=Outer{nested:Inner{text:"replacement",length:11}}
    return "detached"
}
fn main() {
    var copy=global
    copy.nested.text="local"
    print(copy.nested.text,global.nested.text,sample().nested.length)
    global.nested.text=replace()
    print(global.nested.text)
}
""",
    }, "len\ntext\nlen\ntext\nlocal text 3\nreplacement\n", None),
    "temporary_field": ({
        "main.hua": """struct Inner { field string }
struct Outer { nested Inner }
fn make() Outer { print("make")
    return Outer{nested:Inner{field:"initial"}} }
fn new_value() string { print("rhs")
    return "new" }
fn main(){ make().nested.field=new_value() }
""",
    }, "make\nrhs\n", None),
    "forward_read": ({
        "main.hua": """print("before")
let value=read()
var late=7
fn read() int { return late }
fn main(){print(value)}
""",
    }, "before\n", ("E4001", "4", "24")),
    "strings": ({
        "main.hua": r"""fn main() {
    let text="中文\0tail"
    print(text,text.len,len(text))
    print("quote:\"\\",str(-7))
    print(text=="中文\0tail","a"<"b","b">"a","a"<="a","b">="a",text+"!")
}
""",
    }, "中文\0tail 11 11\nquote:\"\\ -7\ntrue true true true true 中文\0tail!\n", None),
    "init_only": ({
        "main.hua": "if true { let label=\"boot\"\nprint(label)\n}\n",
    }, "boot\n", None),
    "init_failure": ({
        "main.hua": "import stop\nimport later\nprint(\"root\")\nfn main(){print(\"main\")}\n",
        "stop.hua": "print(\"before\")\nfn divide(x int) int {return 8//x}\nlet broken=divide(0)\n",
        "later.hua": "print(\"later\")\n",
    }, "before\n", ("E4004", "2", "30")),
}


def verify_records(host, generator, work):
    results = {}
    for name, (files, output, error) in CASES.items():
        source_dir = work / name / "source"
        source_dir.mkdir(parents=True)
        for filename, text in files.items():
            (source_dir / filename).write_text(text, encoding="utf-8", newline="\n")
        entry = source_dir / "main.hua"
        product = source_dir.parent / "program.exe"
        cpp = product.with_suffix(".cpp")
        command = [sys.executable, DRIVER, entry, "--hua", host, "--generator", generator]
        build = run(command + ["-o", product], work, SUPERVISOR_SECONDS + 5)
        compile_receipt = json.loads(build["stdout"])
        run(command + ["--emit-cpp", "-o", cpp], work, SUPERVISOR_SECONDS + 5)
        assert compile_receipt["cpp_sha256"] == digest(cpp)
        expected_exit = int(error is not None)
        paths = {action: run([host, action, entry], work, expected=expected_exit)
                 for action in ("interpret", "run")}
        source_dir.rename(source_dir.with_name("source-moved"))
        paths["native"] = run([product], work, expected=expected_exit)
        for outcome in paths.values():
            assert outcome["stdout"] == output, outcome
            if error:
                assert diagnostic(outcome["stderr"]) == error, outcome
            else:
                assert not outcome["stderr"], outcome
        results[name] = dict(build=compile_receipt, paths=paths)
        print(name, "three paths agree", flush=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("generator", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--scalar", action="store_true")
    options = parser.parse_args()
    host, generator, work = (p.resolve() for p in (options.host, options.generator, options.out))
    work.mkdir(parents=True, exist_ok=False)
    os.environ["PATH"] = str(host.parent) + os.pathsep + os.environ["PATH"]
    report = {"generator_sha256": digest(generator), "records": verify_records(host, generator, work)}
    if options.scalar:
        report["scalar"] = verify_programs(host, generator, work)
    save(work / "report.json", report)


if __name__ == "__main__":
    main()
