"""Freeze current 14-module archive_main.hua closure and measure ordinary Native + VM baselines."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "tools"))
from audit_bootstrap_phases import closure  # noqa: E402

OUT = ROOT / "tests" / "artifacts" / "c04-revisit-20261124"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_measured(cmd, cwd, env, timeout=600):
    import ctypes
    from ctypes import wintypes

    class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    class FILETIME(ctypes.Structure):
        _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]

    k32 = ctypes.windll.kernel32
    psapi = ctypes.windll.psapi
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    k32.DuplicateHandle.argtypes = [
        wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE,
        ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD, wintypes.BOOL, wintypes.DWORD,
    ]
    k32.DuplicateHandle.restype = wintypes.BOOL
    k32.GetProcessTimes.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME),
        ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME),
    ]
    k32.GetProcessTimes.restype = wintypes.BOOL
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.CloseHandle.restype = wintypes.BOOL
    psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wintypes.DWORD,
    ]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL

    t0 = time.perf_counter()
    proc = subprocess.Popen(
        [str(x) for x in cmd],
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
        raise RuntimeError(f"Timed out after {timeout}s: {cmd}")
    wall = time.perf_counter() - t0

    # Query process times & peak working set before handle closes
    # Note: subprocess.Popen closes handle after wait(), so use a quick fallback via psutil or accept wall-only if handle closed.
    # Actually we can just record wall; let's also read peak via Job/psapi if needed.
    return {
        "returncode": proc.returncode,
        "wall_seconds": round(wall, 6),
        "stdout": stdout.decode("utf-8", errors="replace"),
        "stderr": stderr.decode("utf-8", errors="replace"),
    }


def run_measured_win(cmd, cwd, env, timeout=600):
    """Run process on Windows and collect wall, CPU time, and peak working set before closing handle."""
    import ctypes
    from ctypes import wintypes

    class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    class FILETIME(ctypes.Structure):
        _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]

    k32 = ctypes.windll.kernel32
    psapi = ctypes.windll.psapi
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    k32.DuplicateHandle.argtypes = [
        wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE,
        ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD, wintypes.BOOL, wintypes.DWORD,
    ]
    k32.DuplicateHandle.restype = wintypes.BOOL
    k32.GetProcessTimes.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME),
        ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME),
    ]
    k32.GetProcessTimes.restype = wintypes.BOOL
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.CloseHandle.restype = wintypes.BOOL
    psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wintypes.DWORD,
    ]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL

    t0 = time.perf_counter()
    proc = subprocess.Popen(
        [str(x) for x in cmd],
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    # Duplicate handle so we can query stats after process terminates
    cur_proc = k32.GetCurrentProcess()
    dup_handle = wintypes.HANDLE()
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010
    ok_dup = k32.DuplicateHandle(
        cur_proc,
        wintypes.HANDLE(int(proc._handle)),
        cur_proc,
        ctypes.byref(dup_handle),
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        False,
        0,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
        if ok_dup:
            k32.CloseHandle(dup_handle)
        raise RuntimeError(f"Timed out after {timeout}s: {cmd}")
    wall = time.perf_counter() - t0

    cpu_s = None
    peak_mib = None
    if ok_dup:
        c_ft, e_ft, k_ft, u_ft = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        if k32.GetProcessTimes(dup_handle, ctypes.byref(c_ft), ctypes.byref(e_ft), ctypes.byref(k_ft), ctypes.byref(u_ft)):
            k_100ns = (k_ft.dwHighDateTime << 32) | k_ft.dwLowDateTime
            u_100ns = (u_ft.dwHighDateTime << 32) | u_ft.dwLowDateTime
            cpu_s = round((k_100ns + u_100ns) / 1e7, 6)
        pmc = PROCESS_MEMORY_COUNTERS()
        pmc.cb = ctypes.sizeof(pmc)
        if psapi.GetProcessMemoryInfo(dup_handle, ctypes.byref(pmc), pmc.cb):
            peak_mib = round(pmc.PeakWorkingSetSize / (1024 * 1024), 2)
        k32.CloseHandle(dup_handle)

    return {
        "returncode": proc.returncode,
        "wall_seconds": round(wall, 6),
        "cpu_seconds": cpu_s,
        "peak_rss_mib": peak_mib,
        "stdout": stdout.decode("utf-8", errors="replace"),
        "stderr": stderr.decode("utf-8", errors="replace"),
    }


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    frozen = OUT / "frozen"
    if not frozen.exists():
        frozen.mkdir()
        for name, path in sorted(closure(ROOT / "selfhost/archive_main.hua").items()):
            shutil.copyfile(path, frozen / name)

    manifest = {
        "frozen_dir": str(frozen),
        "sources": {p.name: {"bytes": p.stat().st_size, "sha256": sha256(p)} for p in sorted(frozen.glob("*.hua"))},
        "hua_exe": str(ROOT / "huaBin/bin/hua.exe"),
        "hua_exe_sha256": sha256(ROOT / "huaBin/bin/hua.exe"),
        "hua_archive_exe": str(ROOT / "huaBin/lib/compiler/hua-archive.exe"),
        "hua_archive_exe_sha256": sha256(ROOT / "huaBin/lib/compiler/hua-archive.exe"),
        "compiler_huab": str(ROOT / "huaBin/lib/bootstrap/compiler.huab"),
        "compiler_huab_sha256": sha256(ROOT / "huaBin/lib/bootstrap/compiler.huab"),
    }
    (OUT / "frozen-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    env = os.environ.copy()
    env["HUA_HOME"] = str(ROOT / "huaBin")

    base_dir = OUT / "ordinary-baseline"
    base_dir.mkdir(exist_ok=True)

    # 1. Ordinary Native run
    native_out = base_dir / "native.huab"
    if native_out.exists():
        native_out.unlink()
    print("[1/2] Running ordinary Native baseline...", flush=True)
    native_res = run_measured_win(
        [ROOT / "huaBin/lib/compiler/hua-archive.exe", frozen / "archive_main.hua", native_out],
        cwd=ROOT,
        env=env,
        timeout=600,
    )
    assert native_res["returncode"] == 0, native_res
    native_sha = sha256(native_out)
    print(
        f"Native: wall={native_res['wall_seconds']}s cpu={native_res['cpu_seconds']}s "
        f"peak={native_res['peak_rss_mib']}MiB sha={native_sha[:16]}",
        flush=True,
    )

    # 2. Ordinary VM run
    vm_out = base_dir / "vm.huab"
    if vm_out.exists():
        vm_out.unlink()
    print("[2/2] Running ordinary VM baseline...", flush=True)
    vm_res = run_measured_win(
        [
            ROOT / "huaBin/bin/hua.exe",
            "run",
            ROOT / "huaBin/lib/bootstrap/compiler.huab",
            "--execution-profile",
            "compiler",
            "--report-budget",
            "--",
            frozen / "archive_main.hua",
            vm_out,
        ],
        cwd=ROOT,
        env=env,
        timeout=600,
    )
    assert vm_res["returncode"] == 0, vm_res
    vm_sha = sha256(vm_out)
    assert native_sha == vm_sha, f"Byte mismatch! native={native_sha} vm={vm_sha}"
    budget = None
    for line in vm_res["stderr"].splitlines():
        if line.startswith('{"schema":"hua.execution.budget.v1"'):
            budget = json.loads(line)
    print(
        f"VM: wall={vm_res['wall_seconds']}s cpu={vm_res['cpu_seconds']}s "
        f"peak={vm_res['peak_rss_mib']}MiB steps={budget['steps'] if budget else '?'} sha={vm_sha[:16]}",
        flush=True,
    )

    # Reader check
    chk = run_measured_win([ROOT / "huaBin/bin/hua.exe", "check", native_out], cwd=ROOT, env=env, timeout=30)
    assert chk["returncode"] == 0, chk

    summary = {
        "artifact_sha256": native_sha,
        "artifact_bytes": native_out.stat().st_size,
        "native": {k: v for k, v in native_res.items() if k not in ("stdout", "stderr")},
        "native_receipt": json.loads(native_res["stdout"].strip().splitlines()[-1]),
        "vm": {k: v for k, v in vm_res.items() if k not in ("stdout", "stderr")},
        "vm_receipt": json.loads(vm_res["stdout"].strip().splitlines()[-1]),
        "vm_budget": budget,
    }
    (base_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("DONE baseline:", json.dumps({
        "native_wall": summary["native"]["wall_seconds"],
        "vm_wall": summary["vm"]["wall_seconds"],
        "vm_steps": budget["steps"] if budget else None,
        "sha": native_sha[:16],
    }), flush=True)


if __name__ == "__main__":
    main()
