"""Exercise scalar Tasks through the Hua compiler and the native host frontend."""

import argparse
import json
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

IMPORTS = """
import left as left
import right as right
fn main() {
    print(await left.twice(20),await right.twice(20))
}
"""


def run(exe, cwd, *args):
    return subprocess.run(
        [exe, *args], cwd=cwd, timeout=30,
        capture_output=True, text=True, encoding="utf-8",
    )


def verify(exe, compiler):
    with tempfile.TemporaryDirectory(prefix="Hua scalar Task ") as temporary:
        work = Path(temporary)
        for name, text, expected, modules in (
            ("demo", DEMO, "once\n42\n42\n", {}),
            ("scalars", SCALARS, "true 3 hua!\nping\n", {}),
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
            assert (host.returncode, host.stdout, host.stderr) == (0, expected, ""), host
            built = run(exe, work, "run", compiler, "--execution-profile", "compiler", "--", source, output)
            assert built.returncode == 0, built
            receipt = json.loads(built.stdout)
            assert receipt["status"] == "ok" and receipt["format"] == 6, receipt
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
            ("async fn wrong(xs []int) int { return 1 }\nfn main() {}\n", "E8201@"),
            ("async fn main() {}\n", "E8201@"),
            ("let x = await 42\nfn main() {}\n", "E2004@"),
            ("fn main() { async fn nested() {} }\n", "E8201@"),
        ):
            bad_source.write_text(text, encoding="utf-8")
            rejected = run(exe, work, "run", compiler, "--execution-profile", "compiler", "--",
                           bad_source, bad_output)
            assert rejected.returncode == 0, rejected
            receipt = json.loads(rejected.stdout)
            assert receipt["status"] == "error" and expected in receipt["error"], receipt
            assert bad_output.read_bytes() == b"previous artifact"

    print("Passed 3 positive and 6 negative scalar Task cases")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    verify(args.host.resolve(), args.compiler.resolve())
