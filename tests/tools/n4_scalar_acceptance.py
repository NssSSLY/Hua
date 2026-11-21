"""Build and verify the Hua-owned N4 scalar generator on Windows."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools/hua_native.py"
sys.path.insert(0, str(DRIVER.parent))
from hua_native import CLANG, SUPERVISOR_SECONDS, WASMTIME

CONTROL = """fn mark(value int) int {
    print(value)
    return value
}
fn pair(a int,b int) int { return a*10+b }
fn fib(n int) int {
    if n<2 { return n }
    return fib(n-1)+fib(n-2)
}
fn trap() bool { print(99)
    return true
}
fn main() {
    var step=0
    var total=0
    while step<8 {
        step=step+1
        if step==2 { continue }
        if step==5 { break }
        total=total+step
    }
    if total==8 { let total=7
        print(total)
    } else { print(88) }
    print(pair(mark(3),mark(4)),mark(1)+mark(2),total,fib(7))
    print(false&&trap(),true||trap())
}
"""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run(command, cwd, limit=30, expected=0):
    started = time.perf_counter()
    result = subprocess.run(command, cwd=cwd, timeout=limit, capture_output=True, encoding="utf-8")
    seconds = round(time.perf_counter()-started, 3)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    return dict(seconds=seconds, stdout=result.stdout, stderr=result.stderr, exit_code=result.returncode)


def bootstrap(host, seed, source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    command = [host, "run", seed, "--execution-profile", "compiler", "--report-budget",
               "--", source, target]
    result = run(command, target.parent, SUPERVISOR_SECONDS)
    save(target.with_suffix(".build.json"), result)
    receipt = json.loads(result["stdout"])
    assert receipt["status"] == "ok" and target.stat().st_size == receipt["file_bytes"], receipt
    run([host, "check", target], target.parent)
    print(target.parent.name, "built", result["seconds"], "seconds", flush=True)
    return dict(sha256=digest(target), seconds=result["seconds"], receipt=receipt,
                budget=json.loads(result["stderr"]))


def diagnostic(stderr):
    code = re.search(r"error\[(E\d+)\]", stderr)
    site = re.search(r"-->\s+.*?:(\d+):(\d+)", stderr)
    assert code and site, stderr
    return (code[1], *site.groups())


def verify_programs(host, generator, work):
    cases = {
        "control": (CONTROL, "7\n3\n4\n1\n2\n34 3 8 13\nfalse true\n", None),
        "numbers": ("fn main() { print(-9223372036854775808,0x7f,0b11,0o10,1_024,-7//2,-7%2,7//-2,7%-2) }\n",
                    "-9223372036854775808 127 3 8 1024 -4 1 -4 -1\n", None),
        "division": ("fn divzero(n int) int { return 3//n }\nfn main() { print(divzero(0)) }\n", "", "E4004"),
        "overflow": ("fn main() { var n=9223372036854775807\nprint(n+1)\n}\n", "", "E4002"),
    }
    results = {}
    for name, (source, output, code) in cases.items():
        path = work / f"{name}.hua"
        path.write_text(source, encoding="utf-8", newline="\n")
        product = path.with_suffix(".exe")
        command = [sys.executable, DRIVER, path, "--hua", host, "--generator", generator]
        built = run(command + ["-o", product], work, SUPERVISOR_SECONDS+5)
        cpp = path.with_suffix(".cpp")
        generated = run(command + ["--emit-cpp", "-o", cpp], work, SUPERVISOR_SECONDS+5)
        assert json.loads(built["stdout"])["cpp_sha256"] == digest(cpp)
        assert json.loads(generated["stdout"])["cpp_sha256"] == digest(cpp)
        checks = {action: run([host, action, path], work, expected=int(code is not None))
                  for action in ("interpret", "run")}
        path.rename(path.with_suffix(".moved"))
        checks["native"] = run([product], work, expected=int(code is not None))
        for outcome in checks.values():
            assert outcome["stdout"] == output, outcome
            if code:
                assert diagnostic(outcome["stderr"])[0] == code, outcome
                assert diagnostic(outcome["stderr"]) == diagnostic(checks["run"]["stderr"]), checks
            else:
                assert not outcome["stderr"], outcome
        results[name] = dict(build=json.loads(built["stdout"]), paths=checks)
        print(name, "three paths agree", flush=True)
    for name, source in {
        "fixed-array": 'fn main() { let values [2]int=[1,2]\nprint(values[0])\n}\n',
        "async": 'async fn worker() int {return 7}\nfn main() {}\n',
    }.items():
        path = work / f"reject-{name}.hua"
        path.write_text(source, encoding="utf-8")
        for suffix in (".cpp", ".exe"):
            product = path.with_suffix(suffix)
            product.write_bytes(b"previous artifact")
            command = [sys.executable, DRIVER, path, "--hua", host, "--generator", generator, "-o", product]
            if suffix == ".cpp":
                command.append("--emit-cpp")
            outcome = run(command, work, SUPERVISOR_SECONDS+5, expected=2)
            assert not outcome["stdout"] and "E8001@" in outcome["stderr"], outcome
            assert product.read_bytes() == b"previous artifact"
        results[name] = outcome
    return results


def inspect(host, generator, entry, work):
    command = [host, "run", generator, "--execution-profile", "compiler", "--report-budget",
               "--", "inventory", entry, "-"]
    result = run(command, work, SUPERVISOR_SECONDS)
    data = json.loads(result["stdout"])
    assert data["schema"] == "hua.native.inventory.v1" and data["nodes"] > 0, data
    assert data["status"] == "ok" and data["first_blocker"].startswith("E8001@"), data
    data["budget"] = json.loads(result["stderr"])
    data["seconds"] = result["seconds"]
    save(work / f"inventory-{entry.stem}.json", data)
    print(entry.name, data["nodes"], "nodes in", result["seconds"], "seconds", flush=True)
    return dict(nodes=data["nodes"], seconds=result["seconds"], first_blocker=data["first_blocker"])


def verify_mutation(host, seed, generator, frozen, work):
    variant = work / "variant"
    shutil.copytree(frozen, variant)
    emitter = variant / "native_cpp.hua"
    text = emitter.read_text(encoding="utf-8")
    old = 'if op=="+" { return "add" }'
    assert text.count(old) == 1
    emitter.write_text(text.replace(old, 'if op=="+" { return "sub" }'), encoding="utf-8", newline="\n")
    changed = work / "changed" / "generator.huab"
    built = bootstrap(host, seed, variant / "native_main.hua", changed)
    probe = work / "mutation.hua"
    probe.write_text("fn main() { print(9+4) }\n", encoding="utf-8")
    results = {}
    for name, compiler, expected in (("original", generator, "13\n"), ("changed", changed, "5\n")):
        product = work / f"{name}.exe"
        result = run([sys.executable, DRIVER, probe, "--hua", host, "--generator", compiler,
                      "-o", product], work, SUPERVISOR_SECONDS+5)
        output = run([product], work)
        assert output["stdout"] == expected and not output["stderr"], output
        cpp = work / f"{name}.cpp"
        run([sys.executable, DRIVER, probe, "--hua", host, "--generator", compiler,
             "--emit-cpp", "-o", cpp], work, SUPERVISOR_SECONDS+5)
        receipt = json.loads(result["stdout"])
        assert receipt["cpp_sha256"] == digest(cpp)
        results[name] = dict(receipt=receipt, output=output["stdout"])
    original = (work / "original.cpp").read_text(encoding="utf-8")
    changed_cpp = (work / "changed.cpp").read_text(encoding="utf-8")
    assert original.count("hn::add(") == 1 and original.replace("hn::add(", "hn::sub(", 1) == changed_cpp
    return dict(generator=built, programs=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("seed", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    host, seed, work = (p.resolve() for p in (args.host, args.seed, args.out))
    work.mkdir(parents=True, exist_ok=False)
    os.environ["PATH"] = str(host.parent)+os.pathsep+os.environ["PATH"]
    artifacts = [host, seed, CLANG, CLANG.with_name("ld.lld.exe"), WASMTIME, host.parent / "libhua_semantics.a",
                 host.parent / "libhua_frontend.a", host.parent / "wasmtime.dll",
                 *sorted(p for p in (ROOT / "include").rglob("*") if p.is_file())]
    hashes = {str(path): digest(path) for path in artifacts}
    frozen = work / "compiler-source"
    shutil.copytree(ROOT / "selfhost", frozen)
    save(work / "inputs.json", dict(sha256=hashes, clang=run([CLANG,"--version"],work)["stdout"],
                                    sources={p.name:digest(p) for p in sorted(frozen.glob("*.hua"))}))
    generator = work / "original" / "generator.huab"
    built = bootstrap(host, seed, frozen / "native_main.hua", generator)
    programs = verify_programs(host, generator, work)
    save(work / "programs.json", programs)
    with ThreadPoolExecutor(max_workers=3) as pool:
        entries = ("archive_main.hua", "native_main.hua")
        inventories = {name: pool.submit(inspect, host, generator, frozen/name, work) for name in entries}
        mutation = pool.submit(verify_mutation, host, seed, generator, frozen, work)
        scans = {name: task.result() for name, task in inventories.items()}
        proof = mutation.result()
    assert hashes == {str(path):digest(path) for path in artifacts}
    save(work / "report.json", dict(generator=built, programs=programs, inventory=scans, mutation=proof))
    print("N4.0/N4.1 accepted", flush=True)


if __name__ == "__main__":
    main()
