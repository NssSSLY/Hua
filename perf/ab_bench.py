#!/usr/bin/env python3
"""交替运行两个hua构建做A/B对比（同一基准内A、B轮流，减少机器负载漂移的影响）。

用法：
  python perf/ab_bench.py --base 旧hua.exe --new 新hua.exe [--repeat 5] [--only 09] [--save 结果.json]

步数必须一致、stdout必须与 .expect 一致，否则以非零退出。真实负载使用 perf/results/compiler.huab（不存在时请先运行 run_bench.py 生成）。
"""
import argparse, json, pathlib, statistics, sys
from run_bench import ROOT, BENCH, run_one


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True)
    parser.add_argument("--new", required=True)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--only", default="")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--save")
    options = parser.parse_args()
    exes = {"base": pathlib.Path(options.base).resolve(), "new": pathlib.Path(options.new).resolve()}
    jobs = [(p.stem, p, (), p.with_suffix(".expect")) for p in sorted(BENCH.glob("*.hua"))]
    huab = ROOT / "results" / "compiler.huab"
    out_dir = ROOT / "results" / "out"
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, source in json.loads((ROOT / "workloads.json").read_text("utf-8")).items():
        jobs.append((name, huab, (str(ROOT.parent / source), str(out_dir / (name + ".huab"))), ROOT / "bench" / (name + ".expect")))
    jobs = [job for job in jobs if options.only in job[0]]
    print(f"{'基准':<18}{'步数':>12}{'base(s)':>10}{'new(s)':>10}{'加速':>8}")
    results, failed = {}, False
    for name, source, args, expect in jobs:
        walls = {"base": [], "new": []}
        steps = {}
        for _ in range(options.repeat):
            for key in ("base", "new"):
                wall, step, stdout = run_one(exes[key], source, options.timeout, args)
                walls[key].append(wall)
                steps.setdefault(key, step)
                if expect.exists() and expect.read_text("utf-8") != stdout:
                    print(f"{name}: {key} 输出不一致!", flush=True)
                    failed = True
        base, new = statistics.median(walls["base"]), statistics.median(walls["new"])
        note = "" if steps["base"] == steps["new"] else f"  步数变化 {steps['base']}→{steps['new']}"
        failed |= bool(note)
        results[name] = {"steps": steps["new"], "base": base, "new": new, "walls": walls}
        print(f"{name:<18}{steps['new']:>12}{base:>10.3f}{new:>10.3f}{base / new:>7.2f}x{note}", flush=True)
    if options.save:
        pathlib.Path(options.save).write_text(json.dumps({"base": str(exes["base"]), "new": str(exes["new"]), "results": results}, ensure_ascii=False, indent=2), "utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
