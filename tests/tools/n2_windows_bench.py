"""Windows N2 acceptance, fixed-work worker scaling, and File backend comparison."""

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import time
from pathlib import Path

import psutil

import native_build


CPU = """import std.task as task
fn compute(seed int) int {
    let worker=task.worker_id()
    var value=seed
    for i in 0..2500000 {
        value=(value*17+i)%1000003
    }
    task.yield()
    print(worker,worker==task.worker_id())
    return value
}
"""

FILE = """import std.bytes as bytes
import std.file as file
import std.os as os
fn work(path string) int {
    let handle=unwrap(file.open(path,"r"))
    var total=0
    for i in 0..256 {
        let block=unwrap(file.read(handle,65536))
        if !is_none(block) { total+=bytes.len(unwrap(block)) }
    }
    unwrap(file.close(handle))
    return total
}
"""

CONTRACT = r"""import std.bytes as bytes
import std.file as file
import std.task as task
fn exercise() {
    let h=unwrap(file.open("file.txt","w+"))
    let data=bytes.from_text("first\r\nsecond\n")
    print(unwrap(file.write(h,data)),unwrap(file.tell(h)))
    unwrap(file.seek(h,0,"start"))
    print(unwrap(unwrap(file.read_line(h))),unwrap(file.tell(h)))
    print(unwrap(bytes.to_text(unwrap(unwrap(file.read_exact(h,6))))))
    print(bytes.len(unwrap(file.read_all(h))),is_none(unwrap(file.read(h,1))))
    unwrap(file.seek(h,0,"end"))
    unwrap(file.write_all(h,bytes.from_text("end")))
    unwrap(file.flush(h))
    unwrap(file.close(h))
    print(unwrap(file.state(h)))
    let after=unwrap(file.open("file.txt","a+"))
    unwrap(file.write_all(after,bytes.from_text("!")))
    print(unwrap(file.tell(after)))
    unwrap(file.close(after))
}
fn pause() int {
    defer clean()
    let h=unwrap(file.open("payload.bin","r"))
    var total=0
    for i in 0..128 {
        let block=unwrap(file.read(h,65536))
        if !is_none(block) { total+=bytes.len(unwrap(block)) }
        task.yield()
    }
    unwrap(file.close(h))
    return total
}
fn clean(){ print("cleaned") }
fn main(){
    let first=spawn exercise()
    await first
    let pending=spawn pause()
    print(is_err(task.timeout(pending,1)),task.done(pending),task.state(pending))
}
"""


def parallel_source(top, arguments):
    body = ["fn main() {"]
    jobs = []
    for index, args in enumerate(arguments):
        job = f"j{index}"
        jobs.append(job)
        body.append(f"    let {job}=spawn {args}")
    body.append("    print(" + "+".join(f"(await {job})" for job in jobs) + ")")
    body.append("}")
    return top + "\n".join(body) + "\n"


def sample(cmd, directory, env):
    start = time.perf_counter()
    with subprocess.Popen(cmd, cwd=directory, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, encoding="utf-8") as process:
        deadline = start + 30
        peak_threads, peak_rss = 0, 0
        try:
            metrics = psutil.Process(process.pid)
            while process.poll() is None:
                if time.perf_counter() >= deadline:
                    raise subprocess.TimeoutExpired(cmd, 30)
                try:
                    peak_threads = max(peak_threads, metrics.num_threads())
                    peak_rss = max(peak_rss, metrics.memory_info().peak_wset)
                except psutil.AccessDenied:
                    pass
                time.sleep(0.002)
        except psutil.NoSuchProcess:
            pass
        except BaseException:
            process.kill()
            raise
        stdout, stderr = process.communicate(timeout=max(1, deadline - time.perf_counter()))
        assert process.returncode == 0 and not stderr, (cmd, process.returncode, stdout, stderr)
    return dict(ms=(time.perf_counter() - start) * 1000, output=stdout,
                peak_threads=peak_threads, peak_rss=peak_rss)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hua", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--seed", type=Path, help="include a Hua-compiled archive in the File matrix")
    args = parser.parse_args()
    hua, out = args.hua.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, PATH=str(hua.parent) + os.pathsep + os.environ["PATH"])
    payload = out / "payload.bin"
    payload.write_bytes(bytes(range(256)) * 65536)
    reports = {}
    for name, code in (
        ("cpu", parallel_source(CPU, (f"compute({i})" for i in range(8)))),
        ("file", parallel_source(FILE, ['work("payload.bin")'] * 8)),
        ("contract", CONTRACT),
    ):
        source, executable = out / f"{name}.hua", out / f"{name}.exe"
        source.write_text(code, encoding="utf-8", newline="\n")
        status, message = native_build.build(hua, source, executable, count_steps=False)
        assert status == 0, message
        if name == "contract":
            archive = out / "contract.huab"
            subprocess.run([hua, "build", source, "-o", archive], check=True, cwd=out, timeout=30)
            engines = [
                ("ast", [hua, "interpret", source]), ("vm", [hua, "run", source]),
                ("huab", [hua, "run", archive]), ("native", [executable]),
            ]
            if args.seed:
                selfhost = out / "contract-selfhost.huab"
                compiled = subprocess.run([hua, "run", args.seed.resolve(), "--execution-profile",
                                           "compiler", "--", source, selfhost],
                                          check=True, cwd=out, capture_output=True, timeout=30)
                receipt = json.loads(compiled.stdout)
                assert receipt["status"] == "ok" and receipt["format"] == 29, receipt
                subprocess.run([hua, "check", selfhost], check=True, cwd=out, timeout=30)
                engines.append(("selfhost", [hua, "run", selfhost]))
            source.rename(source.with_suffix(".moved"))
            matrix = {}
            for mode in ("blocking", "iocp"):
                for engine, command in engines:
                    if engine in ("ast", "vm") and not source.exists():
                        source.with_suffix(".moved").rename(source)
                    if engine not in ("ast", "vm") and source.exists():
                        source.rename(source.with_suffix(".moved"))
                    run = sample(command, out, dict(env, HUA_TASK_WORKERS="1", HUA_TASK_FILE_BACKEND=mode))
                    assert (out / "file.txt").read_bytes() == b"first\r\nsecond\nend!"
                    assert run["output"].splitlines()[:6] == ["14 14", "first 7", "second", "1 true", "closed", "18"], run
                    assert run["output"].splitlines()[-1] in {"true true canceled","true true completed","false true completed"}, run
                    matrix[f"{mode}-{engine}"] = run
            reports[name] = matrix
            continue
        keys = ["1", "2", "4"] if name == "cpu" else ["blocking", "iocp"]
        samples = {key: [] for key in keys}
        for index in range(7):
            for key in keys[::(-1 if index % 2 else 1)]:
                run = sample([executable], out,
                             dict(env, HUA_TASK_WORKERS=key if name == "cpu" else "4",
                                  HUA_TASK_FILE_BACKEND="iocp" if name == "cpu" else key))
                lines = run.pop("output").splitlines()
                run["checksum"] = int(lines[-1])
                if name == "cpu":
                    assert len(lines) == 9 and all(line.endswith(" true") for line in lines[:-1]), lines
                    run["workers_used"] = len({line.rsplit(" ",1)[0] for line in lines[:-1]})
                    assert run["workers_used"] == int(key), (key, run)
                else:
                    assert run["checksum"] == 8 * payload.stat().st_size
                samples[key].append(run)
        assert len({row["checksum"] for rows in samples.values() for row in rows}) == 1
        summary = {}
        for key, rows in samples.items():
            durations = sorted(row["ms"] for row in rows)
            summary[key] = dict(median_ms=statistics.median(durations), p95_ms=durations[-1],
                                peak_threads=max(row["peak_threads"] for row in rows),
                                peak_rss=max(row["peak_rss"] for row in rows))
            if name == "file":
                summary[key]["mib_per_second"] = (8 * payload.stat().st_size / (1024**2)
                                                  / (summary[key]["median_ms"] / 1000))
        assert all(row["peak_threads"] > 0 and row["peak_rss"] > 0 for row in summary.values()), summary
        reports[name] = dict(summary=summary, samples=samples)
    reports["host_sha256"] = hashlib.sha256(hua.read_bytes()).hexdigest()
    (out / "report.json").write_text(json.dumps(reports, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({name: reports[name]["summary"] for name in ("cpu", "file")}, indent=2))


if __name__ == "__main__":
    main()
