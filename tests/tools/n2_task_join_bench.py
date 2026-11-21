"""Compare Task joins on the same Hua input and Windows Release builds.

Both hosts need the native runtime and frontend libraries alongside hua.exe.
The output directory stores executable identities, sources, and paired samples.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

import native_build


PROGRAM = """import std.list as list
import std.task as task
import std.time as clock
fn payload(n int, offset int) List<[]int> {
    let empty [][]int = []
    var rows = list.from_slice(empty)
    for i in 0..n { list.append(rows, [i+offset, i+offset+1]) }
    print("rows", list.len(rows), offset)
    return rows
}
fn main() {
    var ready_at = 0
    taskgroup {
        let first = spawn payload(ROWS, 1)
        let second = spawn payload(ROWS, 2)
        let third = spawn payload(ROWS, 3)
        let fourth = spawn payload(ROWS, 4)
        while !(task.done(first) && task.done(second) && task.done(third) && task.done(fourth)) {
            task.sleep(1)
        }
        ready_at = clock.monotonic_ns()
    }
    print("join_ns", clock.since_ns(ready_at))
}
"""


def run(command, out, env):
    start = time.perf_counter_ns()
    result = subprocess.run(command, cwd=out, env=env, check=True, capture_output=True,
                            encoding="utf-8", timeout=30)
    return result, (time.perf_counter_ns() - start) / 1_000_000


def measure(command, out, env, rows):
    result, elapsed = run(command, out, env)
    lines = result.stdout.splitlines()
    expected = [f"rows {rows} {i}" for i in range(1, 5)]
    assert not result.stderr and lines[:-1] == expected, result
    label, duration = lines[-1].split()
    assert label == "join_ns" and int(duration) >= 0, result
    return {"elapsed_ms": elapsed, "join_ms": int(duration) / 1_000_000}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    hosts = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    dll_path = os.pathsep.join(str(host.parent) for host in hosts.values())
    env = dict(os.environ, PATH=dll_path + os.pathsep + os.environ["PATH"], HUA_TASK_WORKERS="2")
    report = {"host_sha256": {key: hashlib.sha256(host.read_bytes()).hexdigest()
                              for key, host in hosts.items()}, "workers": 2, "cases": {}}
    for rows in (5000, 25000, 75000):
        source = out / f"rows-{rows}.hua"
        source.write_text(PROGRAM.replace("ROWS", str(rows)), encoding="utf-8")
        archive = out / f"rows-{rows}.huab"
        run([hosts["baseline"], "build", source, "-o", archive], out, env)
        native = {}
        for key, host in hosts.items():
            exe = out / f"rows-{rows}-{key}.exe"
            code, error = native_build.build(host, source, exe, count_steps=False)
            assert code == 0, error
            native[key] = [exe]
        for path, commands in (
            ("huab", {key: [host, "run", archive, "--execution-profile", "compiler"]
                      for key, host in hosts.items()}),
            ("native", native),
        ):
            for command in commands.values():
                measure(command, out, env, rows)
            samples = {key: [] for key in commands}
            for i in range(12):
                for key in list(commands)[::(-1 if i % 2 else 1)]:
                    samples[key].append(measure(commands[key], out, env, rows))
            medians = {key: {metric: statistics.median(sample[metric] for sample in records)
                             for metric in ("elapsed_ms", "join_ms")}
                       for key, records in samples.items()}
            report["cases"][f"{path}-{rows}"] = dict(medians=medians, samples=samples)
            print(f"{path}-{rows} {json.dumps(medians)}", flush=True)
            (out / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                             encoding="utf-8")


if __name__ == "__main__":
    main()
