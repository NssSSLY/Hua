#!/usr/bin/env python3
"""微基准与真实负载的工作量计数（性能路线第9节A阶段）。

需要 HUA_INTERNAL_PROFILE 构建（如 tests/build-profile）中的 hua_memory_probe：
  python perf/count_bench.py --probe tests/build-profile/hua_memory_probe.exe --hua tests/build/hua.exe
  python perf/count_bench.py ... --only 09 --save perf/results/win-counts.json

每个基准先用 --hua 构建成HUAB，再由探针执行：输出步数、本机分配次数/步、
指令构成与内建调用前几名。计数构建较慢，墙钟不作性能结论，只看工作量。
"""
import argparse, json, pathlib, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent
BENCH = ROOT / "bench"


def last_json_lines(text):
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return rows


def top(mapping, n):
    total = sum(mapping.values()) or 1
    return [(k, v, 100 * v / total) for k, v in sorted(mapping.items(), key=lambda x: -x[1])[:n]]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, help="hua_memory_probe（或 hua_performance_probe，无分配计数）")
    parser.add_argument("--hua", required=True, help="用于构建基准HUAB的hua")
    parser.add_argument("--only", default="")
    parser.add_argument("--save")
    parser.add_argument("--timeout", type=float, default=3600)
    options = parser.parse_args()
    probe, hua = pathlib.Path(options.probe).resolve(), pathlib.Path(options.hua).resolve()
    out_dir = ROOT / "results" / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    jobs = []
    for source in sorted(BENCH.glob("*.hua")):
        huab = out_dir / (source.stem + ".huab")
        jobs.append((source.stem, source, huab, [], source.with_suffix(".expect")))
    compiler = ROOT / "results" / "compiler.huab"
    for name, source in json.loads((ROOT / "workloads.json").read_text("utf-8")).items():
        jobs.append((name, None, compiler, [str(ROOT.parent / source), str(out_dir / (name + ".counted.huab"))], ROOT / "bench" / (name + ".expect")))
    jobs = [job for job in jobs if options.only in job[0]]
    results, failed = {}, False
    print(f"{'基准':<18}{'步数':>12}{'分配/步':>9}  指令前四 | 内建前三")
    for name, source, huab, args, expect in jobs:
        if source is not None:
            subprocess.run([str(hua), "build", str(source), "-o", str(huab)], check=True, capture_output=True)
        elif not huab.exists():
            raise SystemExit("缺少 perf/results/compiler.huab：先运行 run_bench.py 生成")
        done = subprocess.run([str(probe), "--mix", str(huab), *args], capture_output=True, text=True, encoding="utf-8", timeout=options.timeout)
        rows = last_json_lines(done.stderr)
        if done.returncode != 0 or len(rows) < 2:
            print(f"{name}: 探针失败 exit {done.returncode}\n{done.stderr[-1500:]}")
            failed = True
            continue
        counters, summary = rows[-2], rows[-1]
        if expect.exists() and expect.read_text("utf-8") != done.stdout:
            print(f"{name}: 输出与 .expect 不一致")
            failed = True
        steps = summary["steps"]
        allocations = summary.get("native_allocations")
        ops, builtins = counters.get("ops", {}), counters.get("builtin_calls_by_name", {})
        results[name] = {"steps": steps, "native_allocations": allocations,
                         "allocations_per_step": allocations / steps if allocations is not None and steps else None,
                         "fused_steps": counters.get("fused_steps", 0), "ops": ops, "builtins": builtins,
                         "load_kinds": counters.get("load_kinds", {}), "constant_kinds": counters.get("constant_kinds", {}),
                         "copy_const_kinds": counters.get("copy_const_kinds", {}), "copy_move_kinds": counters.get("copy_move_kinds", {})}
        per = "-" if allocations is None else f"{allocations / steps:.2f}"
        op_text = " ".join(f"{k}{p:.0f}%" for k, _, p in top(ops, 4))
        builtin_text = " ".join(f"{k.replace('$std$', '')}{p:.0f}%" for k, _, p in top(builtins, 3))
        print(f"{name:<18}{steps:>12}{per:>9}  {op_text} | {builtin_text}", flush=True)
    if options.save:
        pathlib.Path(options.save).write_text(json.dumps({"probe": str(probe), "results": results}, ensure_ascii=False, indent=2), "utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
