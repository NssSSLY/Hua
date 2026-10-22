#!/usr/bin/env python3
"""Hua 微基准运行器（Windows / Linux 通用）。

用法：
  python perf/run_bench.py --hua tests/build/hua.exe                # 跑全部，打印表格
  python perf/run_bench.py --hua tests/build/hua.exe --save perf/results/base.json
  python perf/run_bench.py --hua tests/build/hua.exe --compare perf/results/base.json

每个基准用 compiler 配置运行（步数上限足够大），读取 --report-budget 的步数，
取多次运行墙钟的中位数，并计算 ns/步。stdout 必须与 perf/bench/*.expect 一致，
否则视为语义回归并以非零退出。
"""
import argparse, json, pathlib, statistics, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent
BENCH = ROOT / "bench"


def run_one(hua, source, timeout, args=()):
    command = [str(hua), "run", str(source), "--execution-profile", "compiler", "--report-budget"]
    if args:
        command += ["--", *args]
    start = time.perf_counter()
    done = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=timeout)
    wall = time.perf_counter() - start
    steps = None
    for line in done.stderr.splitlines():
        if line.startswith("{") and "hua.execution.budget.v1" in line:
            steps = json.loads(line)["steps"]
    if done.returncode != 0:
        raise RuntimeError(f"{source.name}: exit {done.returncode}\n{done.stderr[-2000:]}")
    return wall, steps, done.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hua", required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--only", default="")
    parser.add_argument("--save")
    parser.add_argument("--compare")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--label", default="")
    parser.add_argument("--compiler-huab", help="自举编译器HUAB；缺省时用被测hua从selfhost/archive_main.hua构建（约2分钟）")
    parser.add_argument("--no-real", action="store_true", help="跳过真实编译负载")
    options = parser.parse_args()
    hua = pathlib.Path(options.hua).resolve()
    jobs = [(p.stem, p, (), p.with_suffix(".expect")) for p in sorted(BENCH.glob("*.hua"))]
    if not options.no_real:
        huab = pathlib.Path(options.compiler_huab) if options.compiler_huab else ROOT / "results" / "compiler.huab"
        if not huab.exists():
            print(f"构建自举编译器HUAB → {huab}", flush=True)
            huab.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(hua), "build", str(ROOT.parent / "selfhost" / "archive_main.hua"), "-o", str(huab)], check=True, timeout=1800)
        out_dir = ROOT / "results" / "out"
        out_dir.mkdir(parents=True, exist_ok=True)
        for name, source in json.loads((ROOT / "workloads.json").read_text("utf-8")).items():
            jobs.append((name, huab, (str(ROOT.parent / source), str(out_dir / (name + ".huab"))), ROOT / "bench" / (name + ".expect")))
    jobs = [job for job in jobs if options.only in job[0]]
    baseline = json.loads(pathlib.Path(options.compare).read_text("utf-8"))["results"] if options.compare else {}
    results, failed = {}, False
    header = f"{'基准':<18}{'步数':>12}{'墙钟中位(s)':>13}{'ns/步':>9}"
    if baseline:
        header += f"{'基线(s)':>10}{'加速':>8}"
    print(header)
    for name, source, args, expect in jobs:
        walls, steps, stdout = [], None, ""
        for _ in range(options.repeat):
            wall, steps, stdout = run_one(hua, source, options.timeout, args)
            walls.append(wall)
        status = ""
        if expect.exists() and expect.read_text("utf-8") != stdout:
            status = "  输出不一致!"
            failed = True
        median = statistics.median(walls)
        ns = median * 1e9 / steps if steps else 0
        results[name] = {"steps": steps, "wall": median, "walls": walls, "ns_per_step": ns}
        line = f"{name:<18}{steps:>12}{median:>13.3f}{ns:>9.1f}"
        if name in baseline:
            old = baseline[name]["wall"]
            line += f"{old:>10.3f}{old / median:>7.2f}x"
            if baseline[name]["steps"] != steps:
                line += f"  步数变化 {baseline[name]['steps']}→{steps}"
        print(line + status, flush=True)
    if options.save:
        path = pathlib.Path(options.save)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"label": options.label, "hua": str(hua), "results": results}, ensure_ascii=False, indent=2), "utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
