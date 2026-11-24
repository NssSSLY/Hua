"""Exercise native run through a packaged Windows SDK."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
from time import perf_counter


def verify(sdk, work):
    work.mkdir(parents=True)
    project = work / "项目 启动"
    project.mkdir()
    temporary = work / "temporary"
    temporary.mkdir()
    host = sdk / "bin/hua.exe"
    env = os.environ | {"HUA_HOME": str(sdk), "TMP": str(temporary), "TEMP": str(temporary)}
    cache = work / "application-cache"

    def command(args, *, directory=project, cache_root=cache, success=True):
        started = perf_counter()
        result = subprocess.run([host, *args], capture_output=True, encoding="utf-8",
                                env=env | {"HUA_RELEASE_CACHE": str(cache_root)}, cwd=directory, timeout=620)
        assert (result.returncode == 0) == success, (args, result)
        assert list(temporary.glob("hua-run-*")) == [], list(temporary.glob("hua-run-*"))
        return (perf_counter() - started) * 1000, result

    def source(name, text):
        path = project / name
        path.write_bytes(text.encode("utf-8"))
        return path

    print_source = source("入口 有空格.hua", """
import std.os as os
fn main() {
    print(unwrap(os.cwd()))
    for item in os.args() { print("["+item+"]") }
}
""".lstrip())
    arguments = ["--", "", "中文 空格", 'embedded"quote', "ends in slash \\"]
    _, native = command(["run", "--release", print_source, "--", *arguments])
    _, vm = command(["run", print_source, "--", *arguments])
    parts = native.stdout.splitlines()
    assert Path(parts[0]) == project
    assert parts[1:] == ["[" + value + "]" for value in arguments], parts
    assert vm.stdout == native.stdout and not native.stderr, (vm, native)
    assert not list(project.glob("*.exe"))

    # Building an EXE separately sees the same cached program from native run.
    output = project / "output.exe"
    _, built = command(["build", "--release", print_source, "-o", output])
    receipt = json.loads(built.stdout)
    assert all(receipt[key] == 0 for key in ("generate_calls", "compile_calls", "link_calls")), receipt

    failure = source("failure.hua", 'fn main() Result<void,string> { return err("child failure") }\n')
    _, child = command(["run", "--release", failure], success=False)
    assert child.returncode == 1 and "child failure" in child.stderr and not child.stdout, child
    invalid = source("invalid.hua", 'fn main() { print(undefined_name) }\n')
    _, rejected = command(["run", "--release", invalid], success=False)
    assert rejected.returncode == 1 and "undefined_name" in rejected.stderr and not rejected.stdout, rejected
    _, usage = command(["run", "--release"], success=False)
    assert usage.returncode == 2 and "run --release" in usage.stderr, usage

    bench = {
        "hello": ('fn main() { print("hello") }\n', "hello"),
        "compute": ("""
fn main() {
    var state = 1
    var i = 0
    while i < 3000000 {
        state = (state*48271)%2147483647
        i += 1
    }
    print(state)
}
""".lstrip(), "966806951"),
    }
    report = {}
    for name, (code, expected) in bench.items():
        entry = source(name + ".hua", code)
        measurements = {"cold_ms": [], "cached_ms": []}
        for i in range(7):
            for key in measurements:
                elapsed, result = command(["run", "--release", entry], cache_root=work / f"{name}-cache-{i}")
                assert result.stdout.strip() == expected and not result.stderr, result
                measurements[key].append(elapsed)
        report[name] = measurements
        print(name, {key: round(statistics.median(values), 3) for key, values in measurements.items()}, flush=True)
    (work / "native-run.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    verify(args.sdk.resolve(), args.work.resolve())
