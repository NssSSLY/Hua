"""Run the same N2 programs through five Windows execution paths."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "n2-task-records.hua": ("77 7 5 12 7 0\n", None),
    "n2-task-local-async.hua": ("6 5 40\n", None),
    "n2-task-value-wrappers.hua": ("ok(3) ok(7) err(missing)\n", None),
    "n2-task-container-snapshot.hua": ("once before 5\n10 99 1 4\n", None),
    "n2-task-container-results.hua": ("8192 4096 17\nchanged task result\n", None),
    "n2-task-control.hua": (None, None),
    "n2-task-error.hua": ("cleanup\n", "E4004"),
}
ERROR = """import std.task as task
fn cleanup() {
    task.yield()
    print("cleanup")
}
fn main() {
    async fn work(zero int) int {
        defer cleanup()
        task.yield()
        return 3 // zero
    }
    print(await work(0))
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--frontend-reports", action="store_true",
                        help="also publish C05/C06 reports for local async recursion")
    args = parser.parse_args()
    host, compiler, out = (p.resolve() for p in (args.host, args.compiler, args.out))
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, HUA_TASK_WORKERS="1", PATH=str(host.parent) + os.pathsep + os.environ["PATH"])

    def run(command, timeout=30):
        return subprocess.run(command, cwd=out, env=env, capture_output=True,
                              encoding="utf-8", timeout=timeout)

    def require(command, timeout=30):
        result = run(command, timeout)
        result.check_returncode()
        return result

    report = {}
    for name, (expected, code) in CASES.items():
        source = out / name
        if name == "n2-task-error.hua":
            source.write_text(ERROR, encoding="utf-8", newline="\n")
        else:
            source.write_bytes((ROOT / "tests/fixtures" / name).read_bytes())
        host_archive = out / (source.stem + "-host.huab")
        own_archive = source.with_suffix(".huab")
        native = source.with_suffix(".exe")
        require([host, "build", source, "-o", host_archive])
        receipt = json.loads(require([host, "run", compiler, "--execution-profile",
                                      "compiler", "--", source, own_archive]).stdout)
        assert receipt["status"] == "ok", receipt
        require([host, "check", own_archive])
        require([sys.executable, ROOT / "tests/tools/native_build.py", host, source, native],
                timeout=120)
        executions = {"ast": run([host, "interpret", source]),
                      "vm": run([host, "run", source])}
        source.rename(source.with_suffix(".moved"))
        executions.update(host_huab=run([host, "run", host_archive]),
                          hua_huab=run([host, "run", own_archive]), native=run([native]))
        reference = expected if expected is not None else executions["vm"].stdout
        for engine, result in executions.items():
            problem = re.search(r"error\[(E\d+)\]", result.stderr)
            actual = problem.group(1) if problem else None
            assert (result.stdout, actual, result.returncode) == (reference, code, int(code is not None)), (
                name, engine, result)
            assert code or not result.stderr, result
        report[name] = {"format": receipt["format"], "paths": {
            engine: dict(stdout=result.stdout, stderr=result.stderr, exit_code=result.returncode)
            for engine, result in executions.items()}}
        (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                         encoding="utf-8")
        print(name, "five paths agree", flush=True)

    if args.frontend_reports:
        fixture = ROOT / "tests/fixtures/n2-task-local-async.hua"
        for command in ("lower", "emit"):
            product = out / f"local-async-{command}.json"
            require([sys.executable, ROOT / "tools/hua_frontend.py", command, fixture,
                     "--hua", host, "-o", product], timeout=600)
            assert json.loads(product.read_bytes())["status"] == "ok"
            print(command, "local async report published", flush=True)


if __name__ == "__main__":
    main()
