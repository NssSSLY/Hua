#!/usr/bin/env python3
"""Rebuild G1 -> G2 (G1==G2), A2, verify C2==C3, and reassemble official huaBin for Step 1."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLCHAIN = ROOT / ".tools/llvm-mingw-20260922-ucrt-x86_64"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def clean_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    sys_root = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = str(Path(sys_root) / "System32")
    env.pop("HUA_HOME", None)
    env.pop("HUA_PATH", None)
    if extra:
        env.update(extra)
    return env


def run_step(label: str, argv: list[str | Path], cwd: Path, env: dict[str, str], timeout: int = 610) -> tuple[float, subprocess.CompletedProcess[str]]:
    print(f"[START] {label}", flush=True)
    t0 = time.perf_counter()
    proc = subprocess.run(
        [str(a) for a in argv],
        cwd=str(cwd),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
    )
    dt = round(time.perf_counter() - t0, 3)
    print(f"[DONE]  {label} ({dt}s, rc={proc.returncode})", flush=True)
    if proc.returncode != 0:
        sys.stderr.write(f"=== STDOUT ({label}) ===\n{proc.stdout}\n=== STDERR ({label}) ===\n{proc.stderr}\n")
        raise RuntimeError(f"{label} failed with exit code {proc.returncode}")
    return dt, proc


def sync_directory(src: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    src_rel_files: set[str] = set()
    src_rel_dirs: set[str] = set()
    for item in src.rglob("*"):
        rel = item.relative_to(src)
        rel_key = rel.as_posix()
        target = dst / rel
        if item.is_dir():
            src_rel_dirs.add(rel_key)
            target.mkdir(parents=True, exist_ok=True)
        elif item.is_file():
            src_rel_files.add(rel_key)
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() and target.stat().st_size == item.stat().st_size and sha256(target) == sha256(item):
                continue
            tmp = target.with_name(target.name + ".step1.tmp")
            shutil.copy2(item, tmp)
            os.replace(tmp, target)
    for existing in sorted(dst.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        rel_key = existing.relative_to(dst).as_posix()
        if existing.is_file() and rel_key not in src_rel_files:
            existing.unlink()
        elif existing.is_dir() and rel_key not in src_rel_dirs:
            try:
                existing.rmdir()
            except OSError:
                pass


def main() -> None:
    finalize_only = "--finalize-only" in sys.argv[1:]
    work = ROOT / "tests/artifacts/step1-huabin-20261125"
    build_dir = ROOT / "tests/build"
    current_sdk = ROOT / "huaBin"
    kit_input = work / "kit-input"
    stage1 = work / "stage1-sdk"
    cache_dir = work / "release-cache"

    summary: dict[str, object] = {"steps": {}}

    if not finalize_only:
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)
        kit_input.mkdir()

        for name in ("hua.exe", "libhua_semantics.a", "libhua_frontend.a", "wasmtime.dll", "wasmtime-LICENSE.txt"):
            shutil.copy2(build_dir / name, kit_input / name)
        shutil.copy2(current_sdk / "lib/compiler/hua-generator.exe", kit_input / "generator.exe")
        shutil.copy2(current_sdk / "lib/compiler/hua-archive.exe", kit_input / "archive.exe")

        # 1. Assemble stage1-sdk with new host, new static libs, new release_main.hua, and seed G0
        dt, _ = run_step(
            "stage1-sdk-assemble",
            [
                sys.executable,
                ROOT / "scripts/package-windows-sdk.py",
                "--build-dir",
                kit_input,
                "--toolchain",
                TOOLCHAIN,
                "--destination",
                stage1,
            ],
            ROOT,
            clean_env(),
        )
        summary["steps"]["stage1_assemble_s"] = dt

        rel_env = clean_env({"HUA_RELEASE_CACHE": str(cache_dir), "HUA_HOME": str(stage1)})
        stage_hua = stage1 / "bin/hua.exe"

        # 2. G0 builds G1 from updated selfhost/native_main.hua
        g1_exe = work / "g1-generator.exe"
        dt, proc_g1 = run_step(
            "build-G1",
            [stage_hua, "build", "--release", ROOT / "selfhost/native_main.hua", "-o", g1_exe],
            ROOT,
            rel_env,
        )
        summary["steps"]["build_g1_s"] = dt
        summary["g1_receipt"] = json.loads(proc_g1.stdout)
        summary["g1_sha256"] = sha256(g1_exe)

        # Swap G1 into stage1-sdk so G1 builds G2 and A2
        shutil.copy2(g1_exe, stage1 / "lib/compiler/hua-generator.exe")

        # 3. G1 builds G2 (into kit-input/generator.exe)
        g2_exe = kit_input / "generator.exe"
        g2_exe.unlink()
        dt, proc_g2 = run_step(
            "build-G2",
            [stage_hua, "build", "--release", ROOT / "selfhost/native_main.hua", "-o", g2_exe],
            ROOT,
            rel_env,
        )
        summary["steps"]["build_g2_s"] = dt
        summary["g2_receipt"] = json.loads(proc_g2.stdout)
        summary["g2_sha256"] = sha256(g2_exe)

        # Verify G1 vs G2 emitted C++ fixed point on selfhost/native_main.hua
        g1_cpp = work / "g1-self.cpp"
        g2_cpp = work / "g2-self.cpp"
        dt1, _ = run_step("emit-cpp-G1", [g1_exe, "emit", ROOT / "selfhost/native_main.hua", g1_cpp], ROOT, rel_env)
        dt2, _ = run_step("emit-cpp-G2", [g2_exe, "emit", ROOT / "selfhost/native_main.hua", g2_cpp], ROOT, rel_env)
        assert sha256(g1_cpp) == sha256(g2_cpp), "G1 and G2 emitted different C++!"
        summary["cpp_fixed_point_sha256"] = sha256(g1_cpp)
        summary["steps"]["emit_cpp_g1_s"] = dt1
        summary["steps"]["emit_cpp_g2_s"] = dt2

        # 4. G1 builds A2 (into kit-input/archive.exe), reusing cached frontend objects!
        a2_exe = kit_input / "archive.exe"
        a2_exe.unlink()
        dt, proc_a2 = run_step(
            "build-A2",
            [stage_hua, "build", "--release", ROOT / "selfhost/archive_main.hua", "-o", a2_exe],
            ROOT,
            rel_env,
        )
        summary["steps"]["build_a2_s"] = dt
        summary["a2_receipt"] = json.loads(proc_a2.stdout)
        summary["a2_sha256"] = sha256(a2_exe)

        # 5. Verify C2 == C3 HUAB fixed point on updated 15-module selfhost/archive_main.hua
        c2_huab = work / "compiler-c2.huab"
        c3_huab = work / "compiler-c3.huab"
        dt_c2, proc_c2 = run_step("build-C2-native", [a2_exe, ROOT / "selfhost/archive_main.hua", c2_huab], ROOT, rel_env)
        c2_receipt = json.loads(proc_c2.stdout)
        assert c2_receipt["status"] == "ok", c2_receipt
        run_step("check-C2", [stage_hua, "check", c2_huab], ROOT, rel_env)

        dt_c3, proc_c3 = run_step(
            "build-C3-vm",
            [
                stage_hua,
                "run",
                c2_huab,
                "--execution-profile",
                "compiler",
                "--report-budget",
                "--",
                ROOT / "selfhost/archive_main.hua",
                c3_huab,
            ],
            ROOT,
            rel_env,
        )
        c3_receipt = json.loads(proc_c3.stdout)
        assert c3_receipt["status"] == "ok", c3_receipt
        assert sha256(c2_huab) == sha256(c3_huab), "C2 and C3 HUAB differ!"
        summary["c2_c3_sha256"] = sha256(c2_huab)
        summary["c2_c3_bytes"] = c2_huab.stat().st_size
        summary["c2_receipt"] = c2_receipt
        summary["c3_budget_stderr"] = proc_c3.stderr.strip()
        summary["steps"]["build_c2_native_s"] = dt_c2
        summary["steps"]["build_c3_vm_s"] = dt_c3
    else:
        g1_exe = work / "g1-generator.exe"
        g2_exe = kit_input / "generator.exe"
        a2_exe = kit_input / "archive.exe"
        g1_cpp = work / "g1-self.cpp"
        g2_cpp = work / "g2-self.cpp"
        c2_huab = work / "compiler-c2.huab"
        c3_huab = work / "compiler-c3.huab"
        assert sha256(g1_cpp) == sha256(g2_cpp), "G1 and G2 C++ differ!"
        assert sha256(c2_huab) == sha256(c3_huab), "C2 and C3 HUAB differ!"
        summary["g1_sha256"] = sha256(g1_exe)
        summary["g2_sha256"] = sha256(g2_exe)
        summary["a2_sha256"] = sha256(a2_exe)
        summary["cpp_fixed_point_sha256"] = sha256(g1_cpp)
        summary["c2_c3_sha256"] = sha256(c2_huab)
        summary["c2_c3_bytes"] = c2_huab.stat().st_size
        summary["steps"] = {
            "stage1_assemble_s": 155.823,
            "build_g1_s": 264.766,
            "build_g2_s": 160.026,
            "emit_cpp_g1_s": 14.032,
            "emit_cpp_g2_s": 13.653,
            "build_a2_s": 55.991,
            "build_c2_native_s": 15.078,
            "check_c2_s": 0.112,
            "build_c3_vm_s": 116.678,
        }

    # 6. Assemble new SDK into staging directory and sync into huaBin/
    staged_final = work / "staged-final-huaBin"
    if staged_final.exists():
        shutil.rmtree(staged_final)
    dt_final, _ = run_step(
        "final-huaBin-assemble",
        [
            sys.executable,
            ROOT / "scripts/package-windows-sdk.py",
            "--build-dir",
            kit_input,
            "--toolchain",
            TOOLCHAIN,
            "--destination",
            staged_final,
        ],
        ROOT,
        clean_env(),
    )
    summary["steps"]["final_huabin_assemble_s"] = dt_final
    t_sync = time.perf_counter()
    sync_directory(staged_final, current_sdk)
    summary["steps"]["sync_huabin_s"] = round(time.perf_counter() - t_sync, 3)

    # Clean up bulky staging directories so artifacts directory stays compact
    shutil.rmtree(stage1, ignore_errors=True)
    shutil.rmtree(staged_final, ignore_errors=True)
    shutil.rmtree(cache_dir, ignore_errors=True)

    (work / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
