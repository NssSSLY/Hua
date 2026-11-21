"""Exercise Task data through the Hua compiler and the native host frontend."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


DEMO = """
fn main() {
    let first Task<int> = twice(21)
    print(await first)
    print(await first)
}
async fn twice(n int) int {
    print("once")
    return n * 2
}
"""

SCALARS = """
async fn flag(x bool) bool { return !x }
async fn double(x float) float { return x * 2.0 }
async fn text(x string) string { return x + "!" }
async fn ping() { print("ping") }
fn main() {
    print(await flag(false),await double(1.5),await text("hua"))
    await ping()
}
"""

LOCAL_TASK = """
import std.task as task
var setting = 10
fn main() {
    var bias = 4
    var items = [2,3]
    fn child(x int) int {
        task.yield()
        return bias + items[0] + setting + x
    }
    bias = 40
    items[0] = 5
    let job = spawn child(2)
    items[0] = 50
    setting = 100
    print(await job,bias,items[0],setting)
}
"""

LOCAL_SCOPES = """
fn main() {
    let base = 2
    fn recurse(n int) int {
        if n == 0 { return base }
        return 1+recurse(n-1)
    }
    fn outer(k int) int {
        fn inner() int { return base+k }
        return inner()
    }
    print(recurse(3),outer(8))
    {
        let base = 9
        fn recurse(n int) int { return base+n }
        print(recurse(1))
    }
}
"""

IMPORTS = """
import left as left
import right as right
fn main() {
    print(await left.twice(20),await right.twice(20))
}
"""


def run(exe, cwd, *args):
    return subprocess.run(
        [exe, *args], cwd=cwd, timeout=30, env=dict(os.environ,HUA_TASK_WORKERS="1"),
        capture_output=True, text=True, encoding="utf-8",
    )


def verify(exe, compiler):
    with tempfile.TemporaryDirectory(prefix="Hua scalar Task ") as temporary:
        work = Path(temporary)
        for name, text, expected, modules in (
            ("demo", DEMO, "once\n42\n42\n", {}),
            ("scalars", SCALARS, "true 3 hua!\nping\n", {}),
            ("containers", (Path(__file__).parent / "fixtures/n2-task-container-snapshot.hua").read_text(
                encoding="utf-8"), "once before 5\n10 99 1 4\n", {}),
            ("container-results", (Path(__file__).parent / "fixtures/n2-task-container-results.hua").read_text(
                encoding="utf-8"), "8192 4096 17\nchanged task result\n", {}),
            ("value-wrappers", (Path(__file__).parent / "fixtures/n2-task-value-wrappers.hua").read_text(
                encoding="utf-8"), "ok(3) ok(7) err(missing)\n", {}),
            ("task-control", (Path(__file__).parent / "fixtures/n2-task-control.hua").read_text(
                encoding="utf-8"), None, {}),
            ("task-file", """import std.file as file
import std.bytes as bytes
fn worker() int {
    let handle file.File = unwrap(file.open("task-data.txt","w+"))
    unwrap(file.write_all(handle,bytes.from_text("hua")))
    unwrap(file.seek(handle,0,"start"))
    let content = unwrap(file.read_all(handle))
    unwrap(file.close(handle))
    return bytes.len(content)
}
fn main() { print(await spawn worker()) }
""", "3\n", {}),
            ("records", (Path(__file__).parent / "fixtures/n2-task-records.hua").read_text(
                encoding="utf-8"), "77 7 5 12 7 0\n", {}),
            ("local-async", (Path(__file__).parent / "fixtures/n2-task-local-async.hua").read_text(
                encoding="utf-8"), "6 5 40\n", {}),
            ("task-field-forward", "struct Box { job Task<Good> }\nstruct Good { number int }\nfn main() { print(1) }\n", "1\n", {}),
            ("task-field-backward", "struct Good { number int }\nstruct Box { job Task<Good> }\nfn main() { print(1) }\n", "1\n", {}),
            ("local-task", LOCAL_TASK, "21 40 50 100\n", {}),
            ("local-scopes", LOCAL_SCOPES, "5 10\n10\n", {}),
            ("imports", IMPORTS, "40 60\n", {
                "left.hua": "pub async fn twice(x int) int { return x * 2 }\n",
                "right.hua": "pub async fn twice(x int) int { return x * 3 }\n",
            }),
        ):
            source = work / f"{name}.hua"
            output = work / f"{name}.huab"
            source.write_text(text.lstrip(), encoding="utf-8")
            for module, body in modules.items():
                (work / module).write_text(body, encoding="utf-8")
            host = run(exe, work, "run", source)
            assert host.returncode == 0 and not host.stderr, host
            if expected is not None:
                assert host.stdout == expected, host
            built = run(exe, work, "run", compiler, "--execution-profile", "compiler", "--", source, output)
            assert built.returncode == 0, built
            receipt = json.loads(built.stdout)
            expected_format = 29 if name == "task-file" else 6
            assert receipt["status"] == "ok" and receipt["format"] == expected_format, receipt
            checked = run(exe, work, "check", output)
            assert checked.returncode == 0, checked
            for file in (source, *(work / module for module in modules)):
                file.rename(file.with_suffix(".moved"))
            executed = run(exe, work, "run", output)
            assert (executed.returncode, executed.stdout, executed.stderr) == (0, host.stdout, ""), executed

        bad_source = work / "rejected.hua"
        bad_output = work / "old.huab"
        bad_output.write_bytes(b"previous artifact")
        for text, expected in (
            ("fn main() { print(await 42) }\n", "E3004@1:19"),
            ('async fn wrong() int { return "text" }\nfn main() {}\n', "E3004@"),
            ("async fn wrong(xs []Buffer) int { return 1 }\nfn main() {}\n", "E8201@"),
            ("async fn wrong(xs mut []int) {}\nfn main() {}\n", "E8201@"),
            ("async fn wrong(xs [][]int) { xs[0][0]=1 }\nfn main() {}\n", "E3003@"),
            ("async fn main() {}\n", "E8201@"),
            ("import std.task as task\nasync fn work() {}\nfn main() { task.timeout(work(),1) }\n", "E3004@"),
            ("let x = await 42\nfn main() {}\n", "E2004@"),
            ("fn main() { async fn nested(xs mut []int) {} }\n", "E8201@"),
            ("struct Box { out Buffer }\nasync fn bad(x Box) int { return 0 }\nfn main(){}", "E8201@"),
            ("enum Box { Plain(int), Complex(Buffer) }\nasync fn bad(x Box) int { return 0 }\nfn main(){}", "E8201@"),
            ("struct Box { job Task<Bad> }\nstruct Bad { bytes Buffer }\nfn main() {}", "E8201@"),
            ("struct Bad { bytes Buffer }\nstruct Box { job Task<Bad> }\nfn main() {}", "E8201@"),
            ("fn main() { var x=1\nfn child() { x=2 }\n}", "E3003@"),
            ("fn main() { var xs=[1]\nfn child() { xs[0]=2 }\n}", "E3003@"),
            ("fn main() { { fn child() {} }\nchild()\n}", "E3001@"),
            ("fn main() { for i in 0..1 { fn child() { break } } }\n", "E2004@"),
        ):
            bad_source.write_text(text, encoding="utf-8")
            rejected = run(exe, work, "run", compiler, "--execution-profile", "compiler", "--",
                           bad_source, bad_output)
            assert rejected.returncode == 0, rejected
            receipt = json.loads(rejected.stdout)
            assert receipt["status"] == "error" and expected in receipt["error"], receipt
            assert bad_output.read_bytes() == b"previous artifact"

    print("Passed 14 positive and 17 negative Task cases")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    verify(args.host.resolve(), args.compiler.resolve())
