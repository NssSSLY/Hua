"""Steps 6 & 7 of final acceptance: exact C04-only phase wall timing (Baseline vs Candidate on both Native and VM) using std.fs.write_text mtime stamps (works identically in hua build --release Native and VM!)."""
import json
import os
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


def make_fs_stamped_copy(src_dir: Path, dst_dir: Path, stamp_prefix: Path):
    if dst_dir.exists():
        shutil.rmtree(dst_dir)
    shutil.copytree(src_dir, dst_dir)
    pfx = stamp_prefix.as_posix()
    am = (dst_dir / "archive_main.hua").read_text(encoding="utf-8")
    if "import std.fs as pfs" not in am:
        am = "import std.fs as pfs\n" + am
    am = am.replace(
        "fn main() {",
        f'fn main() {{\n    unwrap(pfs.write_text("{pfx}.total_start","1"))',
        1,
    )
    am = am.replace(
        "    print(receipt)",
        f'    print(receipt)\n    unwrap(pfs.write_text("{pfx}.total_end","1"))',
        1,
    )
    (dst_dir / "archive_main.hua").write_text(am, encoding="utf-8", newline="\n")

    tp = (dst_dir / "types.hua").read_text(encoding="utf-8")
    if "import std.fs as pfs" not in tp:
        tp = "import std.fs as pfs\n" + tp
    marker = "    let capabilities=flow.analyze(graph,m.refs,m.declarations,m.fields,flow_functions,flow_variants)?"
    assert tp.count(marker) == 1
    tp = tp.replace(
        marker,
        f'    unwrap(pfs.write_text("{pfx}.c04_start","1"))\n'
        + marker
        + f'\n    unwrap(pfs.write_text("{pfx}.c04_end","1"))',
        1,
    )
    (dst_dir / "types.hua").write_text(tp, encoding="utf-8", newline="\n")


def read_stamps(prefix: Path):
    t0 = Path(str(prefix) + ".total_start").stat().st_mtime_ns
    t1 = Path(str(prefix) + ".c04_start").stat().st_mtime_ns
    t2 = Path(str(prefix) + ".c04_end").stat().st_mtime_ns
    t3 = Path(str(prefix) + ".total_end").stat().st_mtime_ns
    return {
        "c04_seconds": round((t2 - t1) / 1e9, 3),
        "non_c04_seconds": round(((t3 - t0) - (t2 - t1)) / 1e9, 3),
        "marked_total_seconds": round((t3 - t0) / 1e9, 3),
    }


def clear_stamps(prefix: Path):
    for suf in (".total_start", ".c04_start", ".c04_end", ".total_end"):
        Path(str(prefix) + suf).unlink(missing_ok=True)


def main():
    frozen = OUT / "frozen"
    base = json.loads((OUT / "ordinary-baseline/summary.json").read_text(encoding="utf-8"))
    env = os.environ.copy()
    env["HUA_HOME"] = str(ROOT / "huaBin")
    env["HUA_RELEASE_CACHE"] = str(OUT / "release-cache")
    hua = ROOT / "huaBin/bin/hua.exe"
    c1_exe = ACC / "c1-archive.exe"

    # Candidate instrumented copy
    cand_self_tmp = ACC / "cand-self-src"
    if cand_self_tmp.exists():
        shutil.rmtree(cand_self_tmp)
    cand_self_tmp.mkdir(parents=True)
    for f in (ROOT / "selfhost").glob("*.hua"):
        shutil.copy2(f, cand_self_tmp / f.name)

    cand_pfx = ACC / "stamp-cand"
    cand_inst = ACC / "cand-phase-src"
    make_fs_stamped_copy(cand_self_tmp, cand_inst, cand_pfx)

    print("[1/4] Building & running Candidate phase-stamped Native...", flush=True)
    cand_exe = ACC / "cand-phase.exe"
    b = subprocess.run([str(hua), "build", "--release", str(cand_inst / "archive_main.hua"), "-o", str(cand_exe)],
                       cwd=str(ROOT), env=env, capture_output=True, text=True, encoding="utf-8", timeout=300)
    assert b.returncode == 0, b.stderr[-2000:]
    clear_stamps(cand_pfx)
    out_p = ACC / "cand-phase-nat.huab"
    out_p.unlink(missing_ok=True)
    r_cn = run_measured_win([cand_exe, frozen / "archive_main.hua", out_p], cwd=ROOT, env=env, timeout=300)
    assert r_cn["returncode"] == 0 and sha256(out_p) == base["artifact_sha256"]
    cand_nat_phase = read_stamps(cand_pfx)
    print(f"PHASE Candidate Native: {cand_nat_phase} (wall={r_cn['wall_seconds']:.3f}s)", flush=True)

    print("[2/4] Building & running Candidate phase-stamped VM...", flush=True)
    cand_huab = ACC / "cand-phase.huab"
    cand_huab.unlink(missing_ok=True)
    b2 = run_measured_win([c1_exe, cand_inst / "archive_main.hua", cand_huab], cwd=ROOT, env=env, timeout=300)
    assert b2["returncode"] == 0
    clear_stamps(cand_pfx)
    out_p = ACC / "cand-phase-vm.huab"
    out_p.unlink(missing_ok=True)
    r_cv = run_measured_win(
        [hua, "run", cand_huab, "--execution-profile", "compiler", "--report-budget", "--",
         frozen / "archive_main.hua", out_p],
        cwd=ROOT, env=env, timeout=600,
    )
    assert r_cv["returncode"] == 0 and sha256(out_p) == base["artifact_sha256"]
    cand_vm_phase = read_stamps(cand_pfx)
    print(f"PHASE Candidate VM:     {cand_vm_phase} (wall={r_cv['wall_seconds']:.3f}s)", flush=True)

    # Baseline instrumented copy
    base_pfx = ACC / "stamp-base"
    base_inst = ACC / "base-phase-src"
    make_fs_stamped_copy(frozen, base_inst, base_pfx)

    print("[3/4] Building & running Baseline phase-stamped Native...", flush=True)
    base_exe = ACC / "base-phase.exe"
    b3 = subprocess.run([str(hua), "build", "--release", str(base_inst / "archive_main.hua"), "-o", str(base_exe)],
                        cwd=str(ROOT), env=env, capture_output=True, text=True, encoding="utf-8", timeout=300)
    assert b3.returncode == 0, b3.stderr[-2000:]
    clear_stamps(base_pfx)
    out_p = ACC / "base-phase-nat.huab"
    out_p.unlink(missing_ok=True)
    r_bn = run_measured_win([base_exe, frozen / "archive_main.hua", out_p], cwd=ROOT, env=env, timeout=300)
    assert r_bn["returncode"] == 0 and sha256(out_p) == base["artifact_sha256"]
    base_nat_phase = read_stamps(base_pfx)
    print(f"PHASE Baseline Native:  {base_nat_phase} (wall={r_bn['wall_seconds']:.3f}s)", flush=True)

    print("[4/4] Building & running Baseline phase-stamped VM...", flush=True)
    base_huab = ACC / "base-phase.huab"
    base_huab.unlink(missing_ok=True)
    b4 = run_measured_win([c1_exe, base_inst / "archive_main.hua", base_huab], cwd=ROOT, env=env, timeout=300)
    assert b4["returncode"] == 0
    clear_stamps(base_pfx)
    out_p = ACC / "base-phase-vm.huab"
    out_p.unlink(missing_ok=True)
    r_bv = run_measured_win(
        [hua, "run", base_huab, "--execution-profile", "compiler", "--report-budget", "--",
         frozen / "archive_main.hua", out_p],
        cwd=ROOT, env=env, timeout=600,
    )
    assert r_bv["returncode"] == 0 and sha256(out_p) == base["artifact_sha256"]
    base_vm_phase = read_stamps(base_pfx)
    print(f"PHASE Baseline VM:      {base_vm_phase} (wall={r_bv['wall_seconds']:.3f}s)", flush=True)

    c04_nat_x = round(base_nat_phase["c04_seconds"] / cand_nat_phase["c04_seconds"], 3)
    c04_vm_x = round(base_vm_phase["c04_seconds"] / cand_vm_phase["c04_seconds"], 3)
    res = {
        "baseline_native": base_nat_phase,
        "candidate_native": cand_nat_phase,
        "native_c04_speedup": c04_nat_x,
        "baseline_vm": base_vm_phase,
        "candidate_vm": cand_vm_phase,
        "vm_c04_speedup": c04_vm_x,
    }
    (ACC / "phase-ab.json").write_text(json.dumps(res, indent=2) + "\n", encoding="utf-8")
    print("\n=== EXACT C04 PHASE A/B SUMMARY ===", flush=True)
    print(json.dumps(res, indent=2), flush=True)


if __name__ == "__main__":
    main()
