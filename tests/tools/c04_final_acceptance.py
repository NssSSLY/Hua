"""Final Stage-4 Dual-Backend Acceptance for updated selfhost/ (C04 incremental/factored dataflow):
1. Ordinary uninstrumented Native & VM runs on frozen 14-module input (vs baseline).
2. Separate C04-only phase wall timing (Baseline vs Candidate on both Native and VM, frozen 14-module input).
3. Full self-hosting fixed point C1(Native) -> C2 -> C3(VM) on updated 15-module selfhost/, checking C2 == C3 byte-for-byte.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tests" / "artifacts" / "c04-revisit-20261124"
ACC = OUT / "final-acceptance"
sys.path.insert(0, str(ROOT / "tests" / "tools"))
from c04_baseline_freeze import run_measured_win, sha256  # noqa: E402


def parse_c04_phase(stdout: str):
    t0 = t1 = t2 = t3 = None
    for line in stdout.splitlines():
        if line.startswith("@@PHASE "):
            _, tag, val = line.split()
            if tag == "TOTAL_START":
                t0 = int(val)
            elif tag == "C04_START":
                t1 = int(val)
            elif tag == "C04_END":
                t2 = int(val)
            elif tag == "TOTAL_END":
                t3 = int(val)
    assert t1 is not None and t2 is not None, stdout[:500]
    c04_s = (t2 - t1) / 1e9
    total_s = (t3 - t0) / 1e9 if (t0 is not None and t3 is not None) else None
    return {"c04_seconds": round(c04_s, 3), "marked_total_seconds": round(total_s, 3) if total_s else None}


def make_phase_instrumented_copy(src_dir: Path, dst_dir: Path):
    if dst_dir.exists():
        shutil.rmtree(dst_dir)
    shutil.copytree(src_dir, dst_dir)
    # Bracket main() in archive_main.hua
    am = (dst_dir / "archive_main.hua").read_text(encoding="utf-8")
    am = "import std.time as pclk\n" + am
    am = am.replace(
        "fn main() {",
        'fn main() {\n    print("@@PHASE TOTAL_START "+str(pclk.monotonic_ns()))\n    defer print("@@PHASE TOTAL_END "+str(pclk.monotonic_ns()))',
        1,
    )
    (dst_dir / "archive_main.hua").write_text(am, encoding="utf-8", newline="\n")
    # Bracket flow.analyze(...) in types.hua
    tp = (dst_dir / "types.hua").read_text(encoding="utf-8")
    tp = "import std.time as pclk\n" + tp
    marker = "    let capabilities=flow.analyze(graph,m.refs,m.declarations,m.fields,flow_functions,flow_variants)?"
    assert tp.count(marker) == 1
    tp = tp.replace(
        marker,
        '    print("@@PHASE C04_START "+str(pclk.monotonic_ns()))\n'
        + marker
        + '\n    print("@@PHASE C04_END "+str(pclk.monotonic_ns()))',
        1,
    )
    (dst_dir / "types.hua").write_text(tp, encoding="utf-8", newline="\n")


def main():
    ACC.mkdir(parents=True, exist_ok=True)
    frozen = OUT / "frozen"
    base = json.loads((OUT / "ordinary-baseline/summary.json").read_text(encoding="utf-8"))
    env = os.environ.copy()
    env["HUA_HOME"] = str(ROOT / "huaBin")
    env["HUA_RELEASE_CACHE"] = str(OUT / "release-cache")
    hua = ROOT / "huaBin/bin/hua.exe"

    # 1. Build final uninstrumented Native exe from selfhost/
    c1_exe = ACC / "c1-archive.exe"
    print("[1/7] Building c1-archive.exe from selfhost/archive_main.hua...", flush=True)
    t0 = time.perf_counter()
    b = subprocess.run(
        [str(hua), "build", "--release", str(ROOT / "selfhost/archive_main.hua"), "-o", str(c1_exe)],
        cwd=str(ROOT), env=env, capture_output=True, text=True, encoding="utf-8", timeout=300,
    )
    print(f"Build c1-archive.exe rc={b.returncode} time={time.perf_counter()-t0:.2f}s", flush=True)
    assert b.returncode == 0, b.stderr[-2000:]

    # 2. Ordinary uninstrumented Native on frozen 14-module input
    nat_frozen = ACC / "cand-native-frozen.huab"
    nat_frozen.unlink(missing_ok=True)
    print("[2/7] Ordinary Native on frozen 14-module input...", flush=True)
    nat = run_measured_win([c1_exe, frozen / "archive_main.hua", nat_frozen], cwd=ROOT, env=env, timeout=300)
    assert nat["returncode"] == 0, nat
    nat_sha = sha256(nat_frozen)
    assert nat_sha == base["artifact_sha256"], f"Native HUAB mismatch: {nat_sha}"
    chk = subprocess.run([str(hua), "check", str(nat_frozen)], cwd=str(ROOT), env=env, capture_output=True, text=True)
    assert chk.returncode == 0 and chk.stdout.strip() == "OK", (chk.stdout, chk.stderr)
    print(
        f"ORDINARY Native (frozen 14-mod): wall={nat['wall_seconds']:.3f}s (base {base['native']['wall_seconds']:.3f}s, "
        f"{base['native']['wall_seconds']/nat['wall_seconds']:.2f}x) cpu={nat['cpu_seconds']:.3f}s "
        f"peak={nat['peak_rss_mib']}MiB sha={nat_sha[:16]} OK",
        flush=True,
    )

    # 3. C1 -> C2 on updated selfhost/ (15 modules)
    c2_huab = ACC / "c2-selfhost.huab"
    c2_huab.unlink(missing_ok=True)
    print("[3/7] Compiling updated selfhost/ (15-mod) -> C2 via c1-archive.exe...", flush=True)
    c2_run = run_measured_win([c1_exe, ROOT / "selfhost/archive_main.hua", c2_huab], cwd=ROOT, env=env, timeout=300)
    assert c2_run["returncode"] == 0, c2_run
    c2_sha = sha256(c2_huab)
    chk2 = subprocess.run([str(hua), "check", str(c2_huab)], cwd=str(ROOT), env=env, capture_output=True, text=True)
    assert chk2.returncode == 0 and chk2.stdout.strip() == "OK", (chk2.stdout, chk2.stderr)
    print(f"C2 (Native compiling 15-mod selfhost): wall={c2_run['wall_seconds']:.3f}s peak={c2_run['peak_rss_mib']}MiB sha={c2_sha[:16]} OK", flush=True)

    # 4. Ordinary uninstrumented VM (using C2) on frozen 14-module input
    vm_frozen = ACC / "cand-vm-frozen.huab"
    vm_frozen.unlink(missing_ok=True)
    print("[4/7] Ordinary VM (C2) on frozen 14-module input...", flush=True)
    vm = run_measured_win(
        [hua, "run", c2_huab, "--execution-profile", "compiler", "--report-budget", "--",
         frozen / "archive_main.hua", vm_frozen],
        cwd=ROOT, env=env, timeout=600,
    )
    assert vm["returncode"] == 0, vm
    vm_sha = sha256(vm_frozen)
    assert vm_sha == base["artifact_sha256"], f"VM HUAB mismatch: {vm_sha}"
    vm_budget = next(json.loads(l) for l in vm["stderr"].splitlines() if l.startswith('{"schema":"hua.execution.budget.v1"'))
    print(
        f"ORDINARY VM (frozen 14-mod): wall={vm['wall_seconds']:.3f}s (base {base['vm']['wall_seconds']:.3f}s, "
        f"{base['vm']['wall_seconds']/vm['wall_seconds']:.2f}x) cpu={vm['cpu_seconds']:.3f}s "
        f"peak={vm['peak_rss_mib']}MiB steps={vm_budget['steps']:,} ({base['vm_budget']['steps']/vm_budget['steps']:.2f}x) sha={vm_sha[:16]} OK",
        flush=True,
    )

    # 5. Full self-bootstrap C2 -> C3 (VM compiling updated 15-mod selfhost/)
    c3_huab = ACC / "c3-selfhost.huab"
    c3_huab.unlink(missing_ok=True)
    print("[5/7] Self-bootstrap C2 -> C3 (VM compiling 15-mod selfhost)...", flush=True)
    c3_run = run_measured_win(
        [hua, "run", c2_huab, "--execution-profile", "compiler", "--report-budget", "--",
         ROOT / "selfhost/archive_main.hua", c3_huab],
        cwd=ROOT, env=env, timeout=600,
    )
    assert c3_run["returncode"] == 0, c3_run
    c3_sha = sha256(c3_huab)
    c3_budget = next(json.loads(l) for l in c3_run["stderr"].splitlines() if l.startswith('{"schema":"hua.execution.budget.v1"'))
    chk3 = subprocess.run([str(hua), "check", str(c3_huab)], cwd=str(ROOT), env=env, capture_output=True, text=True)
    assert chk3.returncode == 0 and chk3.stdout.strip() == "OK", (chk3.stdout, chk3.stderr)
    assert c2_sha == c3_sha, f"C2 != C3 MISMATCH! {c2_sha} != {c3_sha}"
    print(
        f"C3 (VM compiling 15-mod selfhost): wall={c3_run['wall_seconds']:.3f}s peak={c3_run['peak_rss_mib']}MiB "
        f"steps={c3_budget['steps']:,} sha={c3_sha[:16]} == C2 FIXED POINT MATCH!",
        flush=True,
    )

    # 6. Separate C04-only phase timing (Candidate Native + Candidate VM) on frozen 14-module input
    print("[6/7] Separate C04 phase timing for Candidate (Native + VM) on frozen 14-mod...", flush=True)
    cand_inst = ACC / "cand-phase-src"
    cand_self_tmp = ACC / "cand-self-src"
    if cand_self_tmp.exists():
        shutil.rmtree(cand_self_tmp)
    cand_self_tmp.mkdir(parents=True)
    for f in (ROOT / "selfhost").glob("*.hua"):
        shutil.copy2(f, cand_self_tmp / f.name)
    make_phase_instrumented_copy(cand_self_tmp, cand_inst)

    cand_inst_exe = ACC / "cand-phase.exe"
    b_ci = subprocess.run(
        [str(hua), "build", "--release", str(cand_inst / "archive_main.hua"), "-o", str(cand_inst_exe)],
        cwd=str(ROOT), env=env, capture_output=True, text=True, encoding="utf-8", timeout=300,
    )
    assert b_ci.returncode == 0, b_ci.stderr[-2000:]
    ci_nat_out = ACC / "cand-phase-nat.huab"
    ci_nat_out.unlink(missing_ok=True)
    ci_nat = run_measured_win([cand_inst_exe, frozen / "archive_main.hua", ci_nat_out], cwd=ROOT, env=env, timeout=300)
    assert ci_nat["returncode"] == 0 and sha256(ci_nat_out) == base["artifact_sha256"]
    cand_nat_phase = parse_c04_phase(ci_nat["stdout"])
    print(f"PHASE Candidate Native: C04={cand_nat_phase['c04_seconds']}s / total={cand_nat_phase['marked_total_seconds']}s (wall={ci_nat['wall_seconds']:.3f}s)", flush=True)

    cand_inst_huab = ACC / "cand-phase.huab"
    cand_inst_huab.unlink(missing_ok=True)
    b_ci_vm = run_measured_win([c1_exe, cand_inst / "archive_main.hua", cand_inst_huab], cwd=ROOT, env=env, timeout=300)
    assert b_ci_vm["returncode"] == 0
    ci_vm_out = ACC / "cand-phase-vm.huab"
    ci_vm_out.unlink(missing_ok=True)
    ci_vm = run_measured_win(
        [hua, "run", cand_inst_huab, "--execution-profile", "compiler", "--report-budget", "--",
         frozen / "archive_main.hua", ci_vm_out],
        cwd=ROOT, env=env, timeout=600,
    )
    assert ci_vm["returncode"] == 0 and sha256(ci_vm_out) == base["artifact_sha256"]
    cand_vm_phase = parse_c04_phase(ci_vm["stdout"])
    print(f"PHASE Candidate VM:     C04={cand_vm_phase['c04_seconds']}s / total={cand_vm_phase['marked_total_seconds']}s (wall={ci_vm['wall_seconds']:.3f}s)", flush=True)

    # 7. Separate C04-only phase timing (Baseline Native + Baseline VM) on SAME frozen 14-module input
    print("[7/7] Separate C04 phase timing for Baseline (Native + VM) on frozen 14-mod...", flush=True)
    base_inst = ACC / "base-phase-src"
    make_phase_instrumented_copy(frozen, base_inst)
    base_inst_exe = ACC / "base-phase.exe"
    b_bi = subprocess.run(
        [str(hua), "build", "--release", str(base_inst / "archive_main.hua"), "-o", str(base_inst_exe)],
        cwd=str(ROOT), env=env, capture_output=True, text=True, encoding="utf-8", timeout=300,
    )
    assert b_bi.returncode == 0, b_bi.stderr[-2000:]
    bi_nat_out = ACC / "base-phase-nat.huab"
    bi_nat_out.unlink(missing_ok=True)
    bi_nat = run_measured_win([base_inst_exe, frozen / "archive_main.hua", bi_nat_out], cwd=ROOT, env=env, timeout=300)
    assert bi_nat["returncode"] == 0 and sha256(bi_nat_out) == base["artifact_sha256"]
    base_nat_phase = parse_c04_phase(bi_nat["stdout"])
    print(f"PHASE Baseline Native:  C04={base_nat_phase['c04_seconds']}s / total={base_nat_phase['marked_total_seconds']}s (wall={bi_nat['wall_seconds']:.3f}s)", flush=True)

    base_inst_huab = ACC / "base-phase.huab"
    base_inst_huab.unlink(missing_ok=True)
    b_bi_vm = run_measured_win([c1_exe, base_inst / "archive_main.hua", base_inst_huab], cwd=ROOT, env=env, timeout=300)
    assert b_bi_vm["returncode"] == 0
    bi_vm_out = ACC / "base-phase-vm.huab"
    bi_vm_out.unlink(missing_ok=True)
    bi_vm = run_measured_win(
        [hua, "run", base_inst_huab, "--execution-profile", "compiler", "--report-budget", "--",
         frozen / "archive_main.hua", bi_vm_out],
        cwd=ROOT, env=env, timeout=600,
    )
    assert bi_vm["returncode"] == 0 and sha256(bi_vm_out) == base["artifact_sha256"]
    base_vm_phase = parse_c04_phase(bi_vm["stdout"])
    print(f"PHASE Baseline VM:      C04={base_vm_phase['c04_seconds']}s / total={base_vm_phase['marked_total_seconds']}s (wall={bi_vm['wall_seconds']:.3f}s)", flush=True)

    c04_nat_speedup = round(base_nat_phase["c04_seconds"] / cand_nat_phase["c04_seconds"], 3)
    c04_vm_speedup = round(base_vm_phase["c04_seconds"] / cand_vm_phase["c04_seconds"], 3)
    full_nat_speedup = round(base["native"]["wall_seconds"] / nat["wall_seconds"], 3)
    full_vm_speedup = round(base["vm"]["wall_seconds"] / vm["wall_seconds"], 3)
    vm_steps_speedup = round(base["vm_budget"]["steps"] / vm_budget["steps"], 3)

    summary = {
        "frozen_artifact_sha256": nat_sha,
        "selfhost_c2_sha256": c2_sha,
        "selfhost_c3_sha256": c3_sha,
        "c2_equals_c3": c2_sha == c3_sha,
        "ordinary_baseline": base,
        "ordinary_candidate": {
            "native": {k: v for k, v in nat.items() if k not in ("stdout", "stderr")},
            "vm": {k: v for k, v in vm.items() if k not in ("stdout", "stderr")},
            "vm_budget": vm_budget,
        },
        "selfhost_15mod": {
            "c2_native": {k: v for k, v in c2_run.items() if k not in ("stdout", "stderr")},
            "c3_vm": {k: v for k, v in c3_run.items() if k not in ("stdout", "stderr")},
            "c3_vm_budget": c3_budget,
        },
        "phase_timing_frozen": {
            "baseline_native": base_nat_phase,
            "candidate_native": cand_nat_phase,
            "baseline_vm": base_vm_phase,
            "candidate_vm": cand_vm_phase,
        },
        "speedups": {
            "native_full_wall": full_nat_speedup,
            "native_c04_wall": c04_nat_speedup,
            "vm_full_wall": full_vm_speedup,
            "vm_c04_wall": c04_vm_speedup,
            "vm_total_steps": vm_steps_speedup,
        },
    }
    (ACC / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("\n=== FINAL ACCEPTANCE SUMMARY ===", flush=True)
    print(json.dumps(summary["speedups"], indent=2), flush=True)


if __name__ == "__main__":
    main()
