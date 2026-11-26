#!/usr/bin/env python3
"""Step 2 four-path parity tests for 8 stdlib modules (math, encoding, time, crypto, log, test, gc, process)."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def clean_env(sdk: Path, extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    sys_root = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = str(Path(sys_root) / "System32")
    env["HUA_HOME"] = str(sdk)
    env.pop("HUA_PATH", None)
    if extra:
        env.update(extra)
    return env


def run(argv: list[str | Path], cwd: Path, env: dict[str, str], timeout: int = 180) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in argv],
        cwd=str(cwd),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
    )


def huab_format(path: Path) -> int:
    data = path.read_bytes()
    assert len(data) >= 32, f"Truncated HUAB: {path}"
    magic, version, abi, size, crc, flags = struct.unpack("<8sIIQII", data[:32])
    assert magic == b"HUAB\r\n\x1a\n" and abi == 1 and flags == 0
    return version


def test_huab_format_gates(sdk: Path, scratch: Path) -> None:
    hua = sdk / "bin/hua.exe"
    archive = sdk / "lib/compiler/hua-archive.exe"
    env = clean_env(sdk)

    cases = [
        ("math_f6", 'import std.math as math\nfn main() { print(unwrap(math.floor(1.5))) }\n', 6),
        ("math_f12", 'import std.math as math\nfn main() { print(math.gcd(12, 18)) }\n', 12),
        ("math_f21", 'import std.math as math\nfn main() { print(math.popcount(7)) }\n', 21),
        ("math_f22", 'import std.math as math\nfn main() { print(unwrap(math.checked_add(1, 2))) }\n', 22),
        ("crypto_f13", 'import std.crypto as crypto\nimport std.bytes as bytes\nfn main() { print(bytes.len(crypto.sha256(bytes.from_text("a")))) }\n', 13),
        ("time_f15", 'import std.time as time\nfn main() { print(time.format_duration(1000)) }\n', 15),
        ("log_f18", 'import std.log as log\nfn main() { log.info("hi") }\n', 18),
        ("test_f18", 'import std.test as test\nfn main() { test.equal(1, 1) }\n', 18),
        ("enc_f12", 'import std.encoding as enc\nimport std.bytes as bytes\nfn main() { print(enc.hex_encode(bytes.from_text("A"))) }\n', 12),
        ("enc_f23", 'import std.encoding as enc\nimport std.bytes as bytes\nfn main() { let cp, n = enc.utf8_decode(bytes.from_text("A"), 0)\n print(cp, n) }\n', 23),
        ("enc_f24", 'import std.encoding as enc\nimport std.bytes as bytes\nfn main() { print(bytes.len(unwrap(enc.pack_uint(42, 2, "le")))) }\n', 24),
        ("proc_f28", 'import std.process as process\nimport std.bytes as bytes\nfn main() { let c, o, e, f = process.run(["cmd"], bytes.from_text(""), 1)\n print(c, bytes.len(o), bytes.len(e), f) }\n', 28),
    ]

    gate_dir = scratch / "format_gates"
    gate_dir.mkdir(parents=True, exist_ok=True)
    for name, src, expected_fmt in cases:
        src_file = gate_dir / f"{name}.hua"
        src_file.write_text(src, encoding="utf-8")
        host_huab = gate_dir / f"{name}_host.huab"
        sh_huab = gate_dir / f"{name}_sh.huab"

        p_host = run([hua, "build", src_file, "-o", host_huab], gate_dir, env)
        assert p_host.returncode == 0, f"{name} host build failed: {p_host.stderr}"
        p_sh = run([archive, src_file, sh_huab], gate_dir, env)
        assert p_sh.returncode == 0, f"{name} selfhost archive failed: {p_sh.stdout} {p_sh.stderr}"

        fmt_host = huab_format(host_huab)
        fmt_sh = huab_format(sh_huab)
        assert fmt_host == expected_fmt, f"{name}: host format {fmt_host} != expected {expected_fmt}"
        assert fmt_sh == expected_fmt, f"{name}: selfhost format {fmt_sh} != expected {expected_fmt}"

        assert run([hua, "check", host_huab], gate_dir, env).returncode == 0
        assert run([hua, "check", sh_huab], gate_dir, env).returncode == 0
    print(f"[PASS] HUAB format gates ({len(cases)}/{len(cases)})", flush=True)


def test_static_type_rejections(sdk: Path, scratch: Path) -> None:
    hua = sdk / "bin/hua.exe"
    archive = sdk / "lib/compiler/hua-archive.exe"
    env = clean_env(sdk, {"HUA_RELEASE_CACHE": str(scratch / "cache")})

    cases = [
        ("math_sin_int", "import std.math as math\nfn main() { let _ = math.sin(1) }\n"),
        ("math_gcd_float", "import std.math as math\nfn main() { let _ = math.gcd(1.5, 2.0) }\n"),
        ("math_popcount_u8", "import std.math as math\nfn main() { let _ = math.popcount(u8(1)) }\n"),
        ("test_equal_mismatch", 'import std.test as test\nfn main() { test.equal(1, "1") }\n'),
    ]

    rej_dir = scratch / "rejections"
    rej_dir.mkdir(parents=True, exist_ok=True)
    for name, src in cases:
        src_file = rej_dir / f"{name}.hua"
        src_file.write_text(src, encoding="utf-8")
        sh_out = rej_dir / f"{name}.huab"
        exe_out = rej_dir / f"{name}.exe"

        p_check = run([hua, "check", src_file], rej_dir, env)
        assert p_check.returncode != 0 and "E3004" in (p_check.stderr + p_check.stdout), f"{name} hua check: {p_check.stderr}"

        p_sh = run([archive, src_file, sh_out], rej_dir, env)
        sh_receipt = json.loads(p_sh.stdout)
        assert sh_receipt.get("status") == "error" and "E3004" in str(sh_receipt.get("error", "")), f"{name} hua-archive: {p_sh.stdout} {p_sh.stderr}"
        assert not sh_out.exists(), f"{name} left stale huab"

        p_rel = run([hua, "build", "--release", src_file, "-o", exe_out], rej_dir, env)
        assert p_rel.returncode != 0 and "E3004" in (p_rel.stderr + p_rel.stdout), f"{name} release: {p_rel.stdout} {p_rel.stderr}"
        assert not exe_out.exists(), f"{name} left stale exe"
    print(f"[PASS] Static E3004 rejections across 3 frontends ({len(cases)}/{len(cases)})", flush=True)


def run_four_paths(sdk: Path, scratch: Path, name: str, src: str, args: list[str], expect_rc: int = 0) -> tuple[str, str]:
    hua = sdk / "bin/hua.exe"
    archive = sdk / "lib/compiler/hua-archive.exe"
    env = clean_env(sdk, {"HUA_RELEASE_CACHE": str(scratch / "cache")})

    work = scratch / name
    work.mkdir(parents=True, exist_ok=True)
    src_file = work / "main.hua"
    src_file.write_text(src, encoding="utf-8")

    # 1. VM from source
    p_vm = run([hua, "run", src_file, "--", *args], work, env)
    assert p_vm.returncode == expect_rc, f"{name} vm rc={p_vm.returncode} stderr={p_vm.stderr}"

    # 2. Host HUAB moved to isolated dir
    host_huab = work / "host.huab"
    assert run([hua, "build", src_file, "-o", host_huab], work, env).returncode == 0
    iso_host = work / "iso_host"
    iso_host.mkdir(exist_ok=True)
    moved_host = iso_host / "app.huab"
    shutil.move(str(host_huab), str(moved_host))
    p_host = run([hua, "run", moved_host, "--", *args], iso_host, env)
    assert p_host.returncode == expect_rc, f"{name} host huab rc={p_host.returncode} stderr={p_host.stderr}"

    # 3. Selfhost HUAB moved to isolated dir
    sh_huab = work / "sh.huab"
    p_sh_build = run([archive, src_file, sh_huab], work, env)
    assert p_sh_build.returncode == 0, f"{name} sh build failed: {p_sh_build.stdout} {p_sh_build.stderr}"
    iso_sh = work / "iso_sh"
    iso_sh.mkdir(exist_ok=True)
    moved_sh = iso_sh / "app.huab"
    shutil.move(str(sh_huab), str(moved_sh))
    p_sh = run([hua, "run", moved_sh, "--", *args], iso_sh, env)
    assert p_sh.returncode == expect_rc, f"{name} sh huab rc={p_sh.returncode} stderr={p_sh.stderr}"

    # 4. Native release EXE moved to isolated dir (clean env without HUA_HOME)
    exe_file = work / "app.exe"
    p_rel_build = run([hua, "build", "--release", src_file, "-o", exe_file], work, env)
    assert p_rel_build.returncode == 0, f"{name} release build failed: {p_rel_build.stdout} {p_rel_build.stderr}"
    iso_exe = work / "iso_exe"
    iso_exe.mkdir(exist_ok=True)
    moved_exe = iso_exe / "app.exe"
    shutil.move(str(exe_file), str(moved_exe))
    exe_env = os.environ.copy()
    exe_env["PATH"] = str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32")
    exe_env.pop("HUA_HOME", None)
    exe_env.pop("HUA_PATH", None)
    p_exe = run([moved_exe, *args], iso_exe, exe_env)
    assert p_exe.returncode == expect_rc, f"{name} exe rc={p_exe.returncode} stderr={p_exe.stderr}"

    assert p_vm.stdout == p_host.stdout == p_sh.stdout == p_exe.stdout, (
        f"{name} stdout mismatch:\nVM:\n{p_vm.stdout}\nHOST:\n{p_host.stdout}\nSH:\n{p_sh.stdout}\nEXE:\n{p_exe.stdout}"
    )
    if expect_rc == 0:
        norm = lambda s: re.sub(r"ts=\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z", "ts=<TS>", s)
        assert norm(p_vm.stderr) == norm(p_host.stderr) == norm(p_sh.stderr) == norm(p_exe.stderr), (
            f"{name} stderr mismatch:\nVM:\n{p_vm.stderr}\nHOST:\n{p_host.stderr}\nSH:\n{p_sh.stderr}\nEXE:\n{p_exe.stderr}"
        )
    else:
        def err_sig(s: str) -> tuple[str, list[str]]:
            first = s.splitlines()[0] if s.splitlines() else ""
            locs = []
            for m in re.finditer(r"(?:-->|at)\s+([^\r\n]+):(\d+):(\d+)", s):
                locs.append(f"{Path(m.group(1)).name}:{m.group(2)}:{m.group(3)}")
            return first, locs
        assert err_sig(p_vm.stderr) == err_sig(p_host.stderr) == err_sig(p_sh.stderr) == err_sig(p_exe.stderr), (
            f"{name} error signature mismatch:\nVM={err_sig(p_vm.stderr)}\nHOST={err_sig(p_host.stderr)}\nSH={err_sig(p_sh.stderr)}\nEXE={err_sig(p_exe.stderr)}"
        )
    print(f"[PASS] 4-path runtime parity: {name} (rc={expect_rc})", flush=True)
    return p_vm.stdout, p_vm.stderr


def test_eight_modules_parity(sdk: Path, scratch: Path, test_process: bool) -> None:
    hua_exe = str((sdk / "bin/hua.exe").resolve())
    src = r'''import std.math as math
import std.encoding as enc
import std.time as time
import std.crypto as crypto
import std.log as log
import std.test as test
import std.gc as gc
import std.process as process
import std.bytes as bytes
import std.strings as strings
import std.os as os

fn main() {
    # 1. std.math
    print("math:", math.gcd(48, 18), math.lcm(12, 18), math.isqrt(81), math.popcount(255), math.leading_zeros(1), math.trailing_zeros(16))
    print("math_rot:", math.rotate_left(1, 3), math.rotate_right(8, 3))
    print("math_checked:", unwrap(math.checked_add(10, 20)), is_err(math.checked_add(9223372036854775807, 1)), math.wrapping_add(9223372036854775807, 1), math.saturating_add(9223372036854775807, 100))
    print("math_const:", unwrap(math.floor(math.pi() * 100.0)), unwrap(math.floor(math.e() * 100.0)), unwrap(math.floor(math.tau() * 100.0)))
    print("math_trig:", unwrap(math.round(unwrap(math.sin(0.0)))), unwrap(math.round(unwrap(math.hypot(3.0, 4.0)))), unwrap(math.round(unwrap(math.cbrt(27.0)))), unwrap(math.round(unwrap(math.sinh(0.0)))))

    # 2. std.encoding
    let raw = bytes.from_text("Hua!")
    let hx = enc.hex_encode(raw)
    let b64 = enc.base64_encode(raw)
    let b64u = enc.base64url_encode(raw)
    print("enc:", hx, unwrap(bytes.to_text(unwrap(enc.hex_decode(hx)))), b64, unwrap(bytes.to_text(unwrap(enc.base64_decode(b64)))), b64u, unwrap(bytes.to_text(unwrap(enc.base64url_decode(b64u)))))
    let u8b = unwrap(enc.utf8_encode(21326))
    let cp, width = enc.utf8_decode(u8b, 0)
    print("utf8:", enc.utf8_valid(u8b), cp, width)
    let p_u = unwrap(enc.pack_uint(65530, 2, "be"))
    let p_i = unwrap(enc.pack_int(-1234, 4, "le"))
    let p_f = unwrap(enc.pack_f64(3.5, "le"))
    print("pack:", unwrap(enc.unpack_uint(p_u, 0, 2, "be")), unwrap(enc.unpack_int(p_i, 0, 4, "le")), unwrap(enc.unpack_f64(p_f, 0, "le")))

    # 3. std.time
    let t0 = time.monotonic_ns()
    let dt = time.since_ns(t0)
    let ts = unwrap(time.parse_rfc3339("2026-11-26T08:30:45Z"))
    let y, mo, d, h, mi, s, ms = time.utc_parts(ts)
    let ts2 = unwrap(time.utc_from_parts(y, mo, d, h, mi, s, ms))
    print("time:", dt >= 0, time.unix_ms() > 0, y, mo, d, h, mi, s, ms, ts == ts2, time.weekday(ts), time.day_of_year(ts), time.is_leap_year(2024), time.days_in_month(2024, 2))
    print("time_fmt:", time.format_rfc3339(ts), time.format_rfc3339_offset(ts, 480), time.format_duration(unwrap(time.parse_duration("1h30m"))))

    # 4. std.crypto
    let dig = crypto.sha256(bytes.from_text("abc"))
    let mac = crypto.hmac_sha256(bytes.from_text("key"), bytes.from_text("msg"))
    let rnd = unwrap(crypto.random_bytes(16))
    let rnd0 = unwrap(crypto.random_bytes(0))
    let uid = unwrap(crypto.uuid4())
    print("crypto:", enc.hex_encode(dig), crypto.equal(dig, dig), crypto.equal(dig, mac), bytes.len(rnd), bytes.len(rnd0), len(uid))

    # 5. std.log
    log.set_level("debug")
    log.debug("dbg")
    log.info("inf")
    log.warn("wrn")
    log.error("err")
    log.with_fields("info", "evt", map[string]string{"mod": "batch2a", "ok": "true"})

    # 6. std.test (passing)
    test.assert(true, "should pass")
    test.equal(123, 123)
    test.not_equal("abc", "xyz")
    print("test_ok: true")

    # 7. std.gc (contract: non-negative stats; do not assert VM == native counts)
    let c0 = gc.collect()
    let l0 = gc.live()
    let a0 = gc.allocated()
    let col0 = gc.collected()
    print("gc_ok:", c0 >= 0, l0 >= 0, a0 >= 0, col0 >= 0)

    # 8. std.process (when enabled via arg)
    let argv = os.args()
    if len(argv) >= 2 && argv[0] == "--run-process" {
        let code, out, errb, failure = process.run([argv[1], "version"], bytes.from_text(""), 10000)
        let out_txt = unwrap(bytes.to_text(out))
        let code2, out2, _, failure2 = process.run_with([argv[1], "version"], bytes.from_text(""), 10000, unwrap(os.cwd()), map[string]string{"HUA_BATCH2A": "1"})
        print("proc:", code, failure, strings.contains(strings.lower_ascii(out_txt), "hua"), bytes.len(errb) == 0, code2, failure2, strings.contains(strings.lower_ascii(unwrap(bytes.to_text(out2))), "hua"))
    }
}
'''
    args = ["--run-process", hua_exe] if test_process else []
    out, err = run_four_paths(sdk, scratch, "batch2a_all8", src, args, expect_rc=0)
    assert "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" in out
    assert "level=debug msg=dbg" in err
    assert "level=info msg=evt mod=batch2a ok=true" in err
    if test_process:
        assert "proc: 0  true true 0  true" in out

    # Negative runtime parity for std.test.fail / std.test.equal failure (E4900)
    fail_src = 'import std.test as test\nfn main() { test.equal(1, 2) }\n'
    _, fail_err = run_four_paths(sdk, scratch, "test_equal_fail", fail_src, [], expect_rc=1)
    assert "E4900" in fail_err


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", default="huaBin", help="Path to Hua SDK directory")
    parser.add_argument("--skip-process-exec", action="store_true", help="Skip spawning child process inside std.process test")
    args = parser.parse_args()

    sdk = Path(args.sdk).resolve()
    with tempfile.TemporaryDirectory(prefix="hua-batch2a-") as tmp:
        scratch = Path(tmp)
        test_huab_format_gates(sdk, scratch)
        test_static_type_rejections(sdk, scratch)
        test_eight_modules_parity(sdk, scratch, test_process=not args.skip_process_exec)
    print("ALL STEP 2 BATCH2A PARITY TESTS PASSED")


if __name__ == "__main__":
    main()
