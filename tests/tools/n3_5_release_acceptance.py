"""Windows release performance, reproducibility and portable SDK acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
from time import perf_counter
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invoke(args, directory, env, timeout=600):
    start = perf_counter()
    value = subprocess.run(args, cwd=directory, env=env, capture_output=True,
                           encoding="utf-8", timeout=timeout)
    return round((perf_counter() - start) * 1000, 3), value


def release_env(sdk, cache):
    env = os.environ.copy()
    env["PATH"] = str(sdk / "bin") + os.pathsep + str(Path(os.environ["SystemRoot"]) / "System32")
    env["HUA_HOME"] = str(sdk)
    env.pop("HUA_PATH", None)
    env["HUA_RELEASE_CACHE"] = str(cache)
    return env


def build(sdk, source, output, cache, *, folder=None):
    env = release_env(sdk, cache)
    elapsed, result = invoke([shutil.which("hua.exe", path=env["PATH"]), "build", "--release", source, "-o", output],
                             folder or source.parent, env, 605)
    assert result.returncode == 0, (source, result.stderr, result.stdout)
    receipt = json.loads(result.stdout)
    return {"wall_ms": elapsed, "bytes": output.stat().st_size, "sha256": digest(output), **receipt}


def write(path, content):
    path.write_bytes(content.encode("utf-8"))
    return path


def executions():
    return {
        "hello": ("main.hua", {"main.hua": 'fn main() { print("Hello, Hua!", true, 42) }\n'}, "Hello, Hua! true 42"),
        "scalar-modules": ("main.hua", {
            "main.hua": 'import calc as calc\nprint("main")\nfn main() { print(calc.result(18)) }\n',
            "calc.hua": 'print("calc")\npub fn result(value int) int { return value*2+6 }\n',
        }, "calc\nmain\n42"),
        "containers": ("main.hua", {
            name: (Path(__file__).resolve().parents[2] / "examples/frontend_modules" / name).read_text(encoding="utf-8")
            for name in ("main.hua", "model.hua")
        }, "2 2"),
        "stdlib": ("main.hua", {"main.hua": 'import std.alg_integer as alg\nfn main() { print(alg.lower_bound([2,4,6],4)) }\n'}, "1"),
    }


def test_artifacts(sdk, work, archive):
    alternate = work / "另一个 SDK 路径" / "huaBin"
    with zipfile.ZipFile(archive) as package:
        assert package.testzip() is None
        package.extractall(alternate.parent)
    contents = json.loads((alternate / "manifest.json").read_bytes())
    release = contents["native_release"]
    files = contents["files"] | json.loads((alternate / release["toolchain_inventory"]).read_bytes())
    for name, info in files.items():
        asset = alternate / name
        assert (asset.stat().st_size, digest(asset)) == (info["bytes"], info["sha256"]), name

    result = {"files": len(files), "zip_bytes": archive.stat().st_size, "zip_sha256": digest(archive)}
    for name, (entry, sources, expected) in executions().items():
        rows = []
        for i, (current, label, mode) in enumerate([
            (sdk, "original", "off"), (alternate, "relocated", "empty"), (alternate, "relocated-clean", "off"),
        ]):
            directory = work / (name + "-" + label)
            directory.mkdir()
            source = {file: write(directory / file, content) for file, content in sources.items()}[entry]
            env = release_env(current, directory / "run-cache")
            host = shutil.which("hua.exe", path=env["PATH"])
            assert Path(host).resolve() == (current / "bin/hua.exe").resolve()
            _, version = invoke([host, "version"], directory, env, 15)
            _, vm = invoke([host, "run", source], directory, env, 30)
            assert version.returncode == vm.returncode == 0 and vm.stdout.strip() == expected, (name, vm)
            output = directory / ("different-name.exe" if i > 0 else "program.exe")
            cache = directory / "empty-cache" if mode == "empty" else "off"
            row = build(current, source, output, cache)
            if i == 1:
                hit = build(current, source, output, cache)
                assert hit["sha256"] == row["sha256"]
                assert not any(hit[key] for key in ("generate_calls","compile_calls","link_calls")), hit
            deployed = work / "只留 EXE"
            deployed.mkdir(exist_ok=True)
            isolated = shutil.copyfile(output, deployed / "run.exe")
            plain = {"PATH": str(Path(os.environ["SystemRoot"]) / "System32"), "SystemRoot": os.environ["SystemRoot"]}
            _, run = invoke([isolated], deployed, plain, 15)
            assert run.returncode == 0 and run.stdout.strip() == expected, (name, run)
            rows.append(row)
        assert len({row["sha256"] for row in rows}) == 1, (name, rows)
        result[name] = rows

    runtime = alternate / "include/hua/native_rt.hpp"
    original = runtime.read_bytes()
    stamp = runtime.stat()
    offset = original.index(b"// Runtime")
    changed = original[:offset + 3] + b"r" + original[offset + 4:]
    assert changed != original and len(changed) == len(original)
    directory = work / "containers-relocated"
    output = directory / "header-edited.exe"
    try:
        runtime.write_bytes(changed)
        os.utime(runtime, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        edited = build(alternate, directory / "main.hua", output, directory / "empty-cache")
        assert edited["compile_calls"] and not edited["pch"], edited
        assert edited["sha256"] == result["containers"][1]["sha256"], edited
    finally:
        runtime.write_bytes(original)
        os.utime(runtime, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    pch = alternate / "include/hua/release_runtime.pch"
    pch.rename(pch.with_suffix(".moved"))
    try:
        missing = build(alternate, directory / "main.hua", output, directory / "empty-cache")
        assert missing["compile_calls"] and not missing["pch"], missing
        assert missing["sha256"] == edited["sha256"], missing
    finally:
        pch.with_suffix(".moved").rename(pch)
    pristine = pch.read_bytes()
    try:
        pch.write_bytes(bytes([pristine[0] ^ 1]) + pristine[1:])
        damaged = build(alternate, directory / "main.hua", output, directory / "empty-cache")
        assert damaged["compile_calls"] and not damaged["pch"], damaged
        assert damaged["sha256"] == edited["sha256"], damaged
    finally:
        pch.write_bytes(pristine)
    result["pch_fallback"] = {"header_edit": edited, "missing_pch": missing, "damaged_pch": damaged}

    project = work / "scalar-modules-relocated"
    dependency = project / "calc.hua"
    before = dependency.read_bytes()
    image = project / "rollback.exe"
    try:
        dependency.write_bytes(before.replace(b"value*2+6", b"value*2+7"))
        edited_scalar = build(alternate, project / "main.hua", image, project / "empty-cache")
        assert edited_scalar["sha256"] != result["scalar-modules"][1]["sha256"], edited_scalar
    finally:
        dependency.write_bytes(before)
    recovered = build(alternate, project / "main.hua", image, project / "empty-cache")
    assert recovered["sha256"] == result["scalar-modules"][1]["sha256"], recovered
    result["rollback"] = recovered
    return result


def benchmark(baseline, candidate, work, rounds):
    report = {}
    for name, (entry, files, _) in executions().items():
        directory = work / name
        directory.mkdir()
        source = {file: write(directory / file, content) for file, content in files.items()}[entry]
        records = {"baseline": [], "candidate": []}
        for i in range(rounds):
            variants = [("baseline", baseline), ("candidate", candidate)]
            if i % 2:
                variants.reverse()
            for label, sdk in variants:
                row = build(sdk, source, directory / "program.exe", directory / f"cache-{label}-{i}")
                records[label].append(row)
        report[name] = records
        for label, rows in records.items():
            times = [row["wall_ms"] for row in rows]
            print(name, label, "median", statistics.median(times), "max", max(times), "bytes", rows[0]["bytes"], flush=True)
        (work / "benchmark.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return report


def compare_diagnostics(baseline, candidate, work):
    cases = {
        "overflow": "fn f(x int) int { return x+1 }\nfn main() { print(f(9223372036854775807)) }\n",
        "zero": "fn f(x int) int { return x//0 }\nfn main() { print(f(5)) }\n",
        "depth": "fn f(n int) int { if n==0 { return 0 }\n return f(n-1) }\nfn main() { print(f(200)) }\n",
        "signed": 'fn main() { print(-7//3,-7%3,7//-3,7%-3,"a"+"b",true,false) }\n',
    }
    for name, content in cases.items():
        source = write(work / (name + ".hua"), content)
        observations = []
        for label, sdk in [("baseline", baseline), ("candidate", candidate)]:
            output = work / (name + "-" + label + ".exe")
            row = build(sdk, source, output, "off")
            if label == "candidate":
                assert row["runtime_profile"] == "scalar", row
            _, value = invoke([output], work, release_env(sdk, "off"), 30)
            observations.append((value.returncode, value.stdout, value.stderr))
        assert observations[0] == observations[1], (name, observations)


def scale_regression(sdk, work):
    from n3_module_build_bench import verify_layout

    work.mkdir(parents=True)
    root = Path(__file__).resolve().parents[2]
    sources = work / "sources"
    shutil.copytree(root / "selfhost", sources, ignore=shutil.ignore_patterns("__pycache__"))
    entry = sources / "native_main.hua"
    cache = work / "cache"
    baseline = work / "generator.exe"
    report = {}
    report["cold"] = build(sdk, entry, baseline, cache)
    assert report["cold"]["layout"] == "modules" and report["cold"]["compile_calls"] == 13, report["cold"]
    print("generator cold", report["cold"]["wall_ms"], flush=True)
    report["hot"] = build(sdk, entry, baseline, cache)
    assert report["hot"]["sha256"] == report["cold"]["sha256"] and report["hot"]["compile_calls"] == 0, report["hot"]
    (work / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    lexer = sources / "lexer.hua"
    marker = "fn failure(code string, line int, column int) string {"
    original = lexer.read_text(encoding="utf-8")
    assert marker in original
    write(lexer, original.replace(marker, marker +
                                  '\n    if code=="E1001" { return code+"@"+str(line)+":"+str(column)+" N3.5" }', 1))
    edited = work / "edited.exe"
    report["leaf-edit"] = build(sdk, entry, edited, cache)
    compiled = [unit["module"] for unit in report["leaf-edit"]["units"] if unit["status"] == "compiled"]
    assert compiled == ["lexer.hua"], report["leaf-edit"]
    invalid = write(work / "invalid.hua", "fn main() { $ }\n")
    diagnostics = []
    for generator in (baseline, edited):
        _, proc = invoke([generator, "emit", invalid, work / "unexpected.cpp"], work, release_env(sdk, "off"), 30)
        assert proc.returncode == 0, proc
        receipt = json.loads(proc.stdout)
        assert receipt["status"] == "error", receipt
        diagnostics.append(receipt["detail"])
    assert "N3.5" in diagnostics[1] and diagnostics[1].replace(" N3.5", "") == diagnostics[0], diagnostics
    print("generator leaf edit", report["leaf-edit"]["wall_ms"], flush=True)
    (work / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    verify_layout(sdk, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--zip", type=Path)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--scale", action="store_true")
    args = parser.parse_args()
    baseline, sdk, work = (path.resolve() for path in (args.baseline, args.sdk, args.work))
    work.mkdir(parents=True)
    compare_diagnostics(baseline, sdk, work)
    if args.zip:
        portable = test_artifacts(sdk, work, args.zip.resolve())
        (work / "portability.json").write_text(json.dumps(portable, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.rounds:
        benchmark(baseline, sdk, work, args.rounds)
    if args.scale:
        scale_regression(sdk, work / "scale")


if __name__ == "__main__":
    main()
