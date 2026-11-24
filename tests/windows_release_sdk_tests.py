"""Exercise the installed Windows release driver and the Hua-owned compiler."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from time import monotonic


def sha(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def verify(sdk, work, readobj, rebuild):
    env = os.environ.copy()
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    env.pop("HUA_HOME", None)
    env.pop("HUA_PATH", None)
    work.mkdir()
    public = work / "源码 多模块"
    public.mkdir()
    standalone = work / "应用 交付"
    standalone.mkdir()
    copied = work / "Hua SDK 移动副本"
    shutil.copytree(sdk, copied)
    manifest_path = copied / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    release = manifest["native_release"]
    inventory = copied / release["toolchain_inventory"]
    assert sha(inventory) == manifest["files"][release["toolchain_inventory"]]["sha256"]
    installer_files = manifest["files"] | json.loads(inventory.read_text(encoding="utf-8"))
    for relative, info in installer_files.items():
        file = copied / relative
        assert file.stat().st_size == info["bytes"] and sha(file) == info["sha256"], relative
    host = copied / "bin/hua.exe"
    report = {"files": len(installer_files), "sdk_bytes": sum(v["bytes"] for v in installer_files.values()),
              "host_sha256": sha(host), "generator_sha256": sha(copied / release["generator"]),
              "archive_sha256": sha(copied / release["archive"]), "commands": {}}

    def run(name, argv, cwd=public, limit=60, expect_success=True):
        start = monotonic()
        (work / (name + ".command.json")).write_text(json.dumps(
            {"argv": list(map(str, argv)), "cwd": str(cwd)}, ensure_ascii=False), encoding="utf-8")
        try:
            result = subprocess.run(argv, cwd=cwd, env=env, timeout=limit, capture_output=True)
        except subprocess.TimeoutExpired as error:
            (work / (name + ".stdout")).write_bytes(error.stdout or b"")
            (work / (name + ".stderr")).write_bytes(error.stderr or b"")
            raise
        (work / (name + ".stdout")).write_bytes(result.stdout)
        (work / (name + ".stderr")).write_bytes(result.stderr)
        report["commands"][name] = {"seconds": round(monotonic() - start, 3), "code": result.returncode}
        assert (result.returncode == 0) == expect_success, (name, result.returncode, result.stderr, result.stdout)
        return result.stdout.decode("utf-8")

    def source(name, contents):
        file = public / name
        file.write_text(contents, encoding="utf-8")
        return file

    def build(name, entry, output=None, **options):
        args = [host, "build", "--release", entry]
        if output is not None:
            args.extend(["-o", output])
        raw = run(name, args, **options)
        if options.get("expect_success", True):
            receipt = json.loads(raw)
            assert receipt["schema"] == "hua.build.release.v1" and receipt["status"] == "ok"
            report["commands"][name].update(receipt)

    def run_exe(name, file, expected):
        shipped = standalone / file.name
        shutil.copyfile(file, shipped)
        imports = run(f"{name}-imports", [readobj, "--coff-imports", shipped], cwd=standalone)
        dlls = [line.strip()[6:] for line in imports.splitlines() if line.startswith("  Name: ")]
        system = {"kernel32.dll", "user32.dll", "bcrypt.dll", "shell32.dll", "ucrtbase.dll"}
        assert dlls and all(d.lower() in system or d.lower().startswith("api-ms-win-crt-") for d in dlls), dlls
        assert run(name, [shipped], cwd=standalone).strip() == expected
        report["commands"][name].update({"bytes": shipped.stat().st_size, "sha256": sha(shipped), "imports": dlls})

    hello = source("你好.hua", 'fn main() { print("hello") }\n')
    build("hello-build", hello.name)
    run_exe("hello-run", hello.with_suffix(".exe"), "hello")
    entry = copied / "examples/frontend_modules/main.hua"
    app = public / "out" / "业务.exe"
    build("modules-build", entry, app)
    run_exe("modules-run", app, "2 2")

    alg = source("标准库.hua", 'import std.alg_integer as alg\nfn main() { print(alg.lower_bound([1,3,5],3)) }\n')
    std_app = public / "stdlib.exe"
    build("stdlib-build", alg, std_app)
    run_exe("stdlib-run", std_app, "1")

    archive = public / "native.huab"
    raw = run("native-huab", [copied / release["archive"], entry, archive])
    assert json.loads(raw)["status"] == "ok"
    run("native-huab-check", [host, "check", archive])
    assert run("native-huab-run", [host, "run", archive]).strip() == "2 2"
    run("legacy-build", [host, "build", hello, "-o", "legacy.huab"])
    assert run("legacy-run", [host, "run", "legacy.huab"]).strip() == "hello"

    bad = source("bad.hua", "fn main() { print(unknown_name) }\n")
    snapshot = sha(app)
    build("language-error", bad, app, expect_success=False)
    assert b"E" in (work / "language-error.stderr").read_bytes() and sha(app) == snapshot
    build("extension-error", hello, "wrong.huab", expect_success=False)
    linker = copied / release["linker"]
    disabled = linker.with_suffix(".disabled")
    linker.rename(disabled)
    try:
        build("missing-linker", hello, app, expect_success=False)
        assert b"missing SDK component" in (work / "missing-linker.stderr").read_bytes()
        assert sha(app) == snapshot
    finally:
        disabled.rename(linker)
    assert not list(public.rglob(".hua-*"))

    bare = work / "缺少驱动" / "bin"
    bare.mkdir(parents=True)
    shutil.copyfile(host, bare / "hua.exe")
    shutil.copyfile(copied / "bin/wasmtime.dll", bare / "wasmtime.dll")
    run("missing-driver", [bare / "hua.exe", "build", "--release", hello], expect_success=False)
    diagnostic = (work / "missing-driver.stderr").read_text(encoding="utf-8")
    assert "release driver missing" in diagnostic and "缺少驱动" in diagnostic, diagnostic

    if rebuild:
        compiler_source = copied / "src/compiler/native_cpp.hua"
        code = compiler_source.read_text(encoding="utf-8")
        previous = 'if op=="+" { return "add" }'
        assert code.count(previous) == 1
        compiler_source.write_text(code.replace(previous, 'if op=="+" { return "sub" }'), encoding="utf-8")
        candidate = copied / "lib/compiler/hua-candidate.exe"
        build("candidate-build", copied / release["generator_entry"], candidate, limit=610)
        probe = source("probe.hua", "fn main() { print(9+4) }\n")
        baseline = public / "baseline.exe"
        build("baseline-build", probe, baseline)
        run_exe("baseline-run", baseline, "13")
        release["generator"] = candidate.relative_to(copied).as_posix()
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        modified = public / "modified.exe"
        build("modified-build", probe, modified)
        run_exe("modified-run", modified, "5")
        assert not list(candidate.parent.glob(".hua-*"))

    (work / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(work / "report.json", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--readobj", required=True, type=Path)
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()
    verify(args.sdk.resolve(), args.work.resolve(), args.readobj.resolve(), args.rebuild)
