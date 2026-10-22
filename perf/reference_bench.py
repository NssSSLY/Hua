#!/usr/bin/env python3
"""与成熟解释器同机对照（性能路线第9节A阶段）。

perf/reference/ 下是与 perf/bench/0X_*.hua 算法相同、输出与 .expect 相同的参照程序：
CPython（0X_*.py）与 Lua 5.4（0X_*.lua）。Lua 默认取 .tools/lua-5.4.7/src/lua.exe
（由 lua.org 官方源码用本仓库 llvm-mingw 构建，见 docs/开发与运行指南.md）。

  python perf/reference_bench.py --hua tests/build/hua.exe [--python python] [--lua lua.exe] [--repeat 5] [--save 结果.json]

同一基准内 Hua 与各参照解释器交替运行，比较整程序墙钟中位数（含进程启动）。
"""
import argparse, json, pathlib, statistics, subprocess, sys, time
from run_bench import ROOT, BENCH, run_one

REFERENCE = ROOT / "reference"
DEFAULT_LUA = ROOT.parent / ".tools" / "lua-5.4.7" / "src" / "lua.exe"


def run_reference(command, source, timeout):
    start = time.perf_counter()
    done = subprocess.run([*command, str(source)], capture_output=True, text=True, encoding="utf-8", timeout=timeout)
    wall = time.perf_counter() - start
    if done.returncode != 0:
        raise RuntimeError(f"{source.name}: exit {done.returncode}\n{done.stderr[-2000:]}")
    return wall, done.stdout.replace("\r\n", "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hua", required=True)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--lua", default=str(DEFAULT_LUA))
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--only", default="")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--save")
    options = parser.parse_args()
    hua = pathlib.Path(options.hua).resolve()
    engines = [("Python", [options.python], ".py")]
    if pathlib.Path(options.lua).exists():
        engines.append(("Lua", [options.lua], ".lua"))
    versions = {}
    for label, command, _ in engines:
        probe = subprocess.run([*command, "-v" if label == "Lua" else "--version"], capture_output=True, text=True)
        versions[label] = (probe.stdout or probe.stderr).strip().splitlines()[0]
    print("参照：" + "；".join(versions.values()))
    header = f"{'基准':<16}{'Hua(s)':>9}"
    for label, _, _ in engines:
        header += f"{label + '(s)':>11}{'Hua/' + label:>12}"
    print(header)
    results, failed = {}, False
    for source in sorted(BENCH.glob("0*.hua")):
        name = source.stem
        if options.only not in name:
            continue
        refs = [(label, command, REFERENCE / (name + suffix)) for label, command, suffix in engines if (REFERENCE / (name + suffix)).exists()]
        if not refs:
            continue
        expect = (BENCH / (name + ".expect")).read_text("utf-8")
        walls = {"Hua": []}
        for _ in range(options.repeat):
            wall, _steps, out = run_one(hua, source, options.timeout)
            walls["Hua"].append(wall)
            failed |= out != expect
            for label, command, path in refs:
                wall, out = run_reference(command, path, options.timeout)
                walls.setdefault(label, []).append(wall)
                if out != expect:
                    print(f"{name}: {label} 输出不一致")
                    failed = True
        medians = {k: statistics.median(v) for k, v in walls.items()}
        results[name] = {"medians": medians, "walls": walls}
        line = f"{name:<16}{medians['Hua']:>9.3f}"
        for label, _, _ in engines:
            if label in medians:
                line += f"{medians[label]:>11.3f}{medians['Hua'] / medians[label]:>11.2f}x"
        print(line, flush=True)
    if failed:
        print("输出与 .expect 不一致")
    if options.save:
        pathlib.Path(options.save).write_text(json.dumps({"hua": str(hua), "references": versions, "results": results}, ensure_ascii=False, indent=2), "utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
