"""Compare Windows Task snapshot, yield and cached small-read workloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

import native_build
from n2_windows_bench import parallel_source


MODULE = """import std.list as list
import std.task as task
fn make_rows(n int) List<int> {
    let empty []int=[]
    var data=list.from_slice(empty)
    for i in 0..n { unwrap(list.append(data,i)) }
    return data
}
let unused=make_rows(100000)
var wanted=11
fn read_wanted() int { return wanted }
fn child() int { task.yield()
    return read_wanted()
}
fn main() {
    var total=0
    for i in 0..32 {
        let job=spawn child()
        wanted+=1
        total+=await job
    }
    print(total,list.len(unused),wanted)
}
"""

YIELD = parallel_source("""import std.task as task
fn worker() int {
    for i in 0..20000 { task.yield() }
    return 1
}
""", ["worker()"] * 8)

FILE = parallel_source("""import std.bytes as bytes
import std.file as file
fn reader() int {
    let handle=unwrap(file.open("payload.bin","r"))
    var total=0
    for i in 0..512 {
        let block=unwrap(file.read(handle,4096))
        if !is_none(block) { total+=bytes.len(unwrap(block)) }
    }
    unwrap(file.close(handle))
    return total
}
""", ["reader()"] * 8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--suite", choices=["module", "yield", "file", "all"], default="all")
    args = parser.parse_args()
    hosts = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, HUA_TASK_FILE_BACKEND="iocp")
    env["PATH"] = os.pathsep.join([str(host.parent) for host in hosts.values()] + [env["PATH"]])
    report = {"hosts": {key: hashlib.sha256(host.read_bytes()).hexdigest()
                        for key, host in hosts.items()}, "cases": {}}
    cases = (
        ("module", MODULE, "848 100000 43\n", (2,)),
        ("yield", YIELD, "8\n", (1, 4)),
        ("file", FILE, "16777216\n", (1, 4)),
    )
    for name, code, expected, counts in cases:
        if args.suite not in (name, "all"):
            continue
        if name == "file":
            (out / "payload.bin").write_bytes(bytes(range(256)) * 8192)
        source = out / (name + ".hua")
        source.write_text(code, encoding="utf-8", newline="\n")
        exes = {key: out / f"{name}-{key}.exe" for key in hosts}
        for key, exe in exes.items():
            status, error = native_build.build(hosts[key], source, exe, count_steps=False)
            assert status == 0, error
        for count in counts:
            env["HUA_TASK_WORKERS"] = str(count)

            def sample(key):
                start = time.perf_counter_ns()
                result = subprocess.run([exes[key]], cwd=out, env=env, timeout=30,
                                        capture_output=True, text=True, encoding="utf-8")
                assert (result.returncode, result.stdout, result.stderr) == (0, expected, ""), result
                return (time.perf_counter_ns() - start) / 1_000_000

            samples = {key: [] for key in exes}
            for key in exes:
                sample(key)
            for repeat in range(12):
                for key in list(exes)[::(-1 if repeat % 2 else 1)]:
                    samples[key].append(sample(key))
            medians = {key: statistics.median(times) for key, times in samples.items()}
            report["cases"][f"{name}-{count}"] = dict(medians_ms=medians, samples_ms=samples)
            print(f"{name}-{count}", medians, flush=True)
            (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
