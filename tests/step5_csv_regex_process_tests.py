#!/usr/bin/env python3
"""Step 5 Wave 1 (5A std.csv + 5B std.regex + 5C std.process.spawn) verification suite.

Usage:
  python tests/step5_csv_regex_process_tests.py --host-only --hua tests/build/hua.exe
  python tests/step5_csv_regex_process_tests.py --sdk huaBin
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CLANG = ROOT / ".tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang.exe"


def run_cmd(cmd: list[str | Path], cwd: Path, env: dict[str, str] | None = None, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(x) for x in cmd],
        cwd=str(cwd),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def assert_ok(proc: subprocess.CompletedProcess[str], label: str) -> str:
    if proc.returncode != 0:
        raise AssertionError(
            f"[{label}] failed with rc={proc.returncode}\n"
            f"STDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
        )
    return proc.stdout


def find_sqlite3_dll() -> Path:
    cand = Path(sys.base_prefix) / "DLLs" / "sqlite3.dll"
    if cand.is_file():
        return cand
    raise FileNotFoundError(f"sqlite3.dll not found at {cand}")


def build_sqlite_package(clang: Path, dst_pkg: Path) -> None:
    shutil.copytree(ROOT / "packages/sqlite", dst_pkg)
    shutil.copy2(find_sqlite3_dll(), dst_pkg / "sqlite3.dll")
    assert_ok(
        run_cmd(
            [clang, "-shared", "-O2", "-I", ROOT / "include", dst_pkg / "sqlite_native.c", "-o", dst_pkg / "sqlite_native.dll"],
            cwd=dst_pkg,
            env=os.environ.copy(),
        ),
        "compile sqlite_native.dll",
    )


def build_regex_and_process_program(python_exe: str) -> str:
    py_escaped = python_exe.replace("\\", "\\\\")
    return r'''import std.bytes as bytes
import std.process as proc
import std.regex as regex
import std.stream as stream
import std.time as time

fn die(msg string) {
    print("FAIL:", msg)
    let r Result<int, string> = err(msg)
    let _ = unwrap(r)
}

fn must_pat(s string) regex.Pattern {
    let r = regex.compile(s)
    if is_err(r) {
        die("regex compile failed: " + s)
    }
    return unwrap(r)
}

fn main() {
    # --- 5B: std.regex ---
    let p_num = must_pat("^\\d{2,4}-[a-z]+$")
    if !regex.is_match(p_num, "2026-hua") { die("p_num should match") }
    if regex.is_match(p_num, "2-hua") { die("p_num should not match 1 digit") }
    if regex.is_match(p_num, "2026-HUA") { die("p_num should not match uppercase") }

    # Negated shorthands \D, \W, \S
    let p_nd = must_pat("^\\D+$")
    if !regex.is_match(p_nd, "abc_中文") { die("p_nd should match non-digits") }
    if regex.is_match(p_nd, "abc1") { die("p_nd should reject digits") }

    let p_nw = must_pat("\\W+")
    if !regex.is_match(p_nw, "a-b") { die("p_nw should match hyphen") }
    if regex.is_match(p_nw, "abc_123") { die("p_nw should not match word chars") }

    let p_ws = must_pat("\\s+")
    let parts = regex.split(p_ws, "alpha   beta\tgamma\ndelta", 0)
    if len(parts) != 4 || parts[0] != "alpha" || parts[3] != "delta" {
        die("regex.split mismatch")
    }

    let p_word = must_pat("[A-Za-z]+\\d+")
    let found = regex.find(p_word, "id=x12, y345!")
    if is_none(found) || unwrap(found) != "x12" {
        die("regex.find mismatch")
    }

    let all = regex.find_all(p_word, "id=x12, y345, z9!", 0)
    if len(all) != 3 || all[0] != "x12" || all[1] != "y345" || all[2] != "z9" {
        die("regex.find_all mismatch")
    }

    let replaced = regex.replace_all(p_word, "a1 + b22 = c333", "NUM")
    if replaced != "NUM + NUM = NUM" {
        die("regex.replace_all mismatch: " + replaced)
    }

    # Invalid pattern returns Err
    if !is_err(regex.compile("[unclosed")) {
        die("invalid regex should fail")
    }

    # ReDoS immunity: (a+)+b against 30 'a' + 'c' must finish in < 500ms
    let p_redos = must_pat("(a+)+b")
    let t0 = time.monotonic_ns()
    let redos_hit = regex.is_match(p_redos, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaac")
    let dt = time.monotonic_ns() - t0
    if redos_hit { die("redos pattern should not match") }
    if dt > 500000000 { die("redos too slow") }

    print("REGEX_OK")

    # --- 5C: std.process.spawn ---
    let py = "''' + py_escaped + r'''"
    var env = map[string]string{}
    let code_echo = "import sys; s = sys.stdin.read(); sys.stdout.write('OUT:' + s.upper()); sys.stderr.write('ERR:' + str(len(s)))"
    let ch = unwrap(proc.start([py, "-c", code_echo], "", env))
    let w_in = proc.stdin(ch)
    let r_out = proc.stdout(ch)
    let r_err = proc.stderr(ch)
    unwrap(stream.write_all(w_in, bytes.from_text("hello-hua")))
    unwrap(stream.close_writer(w_in))
    let out_s = unwrap(bytes.to_text(unwrap(stream.read_all(r_out, 65536))))
    let err_s = unwrap(bytes.to_text(unwrap(stream.read_all(r_err, 65536))))
    let rc = unwrap(proc.wait(ch, 5000))
    if rc != 0 || out_s != "OUT:HELLO-HUA" || err_s != "ERR:9" {
        die("echo mismatch: out=" + out_s + " err=" + err_s)
    }

    # Non-zero exit code
    let ch2 = unwrap(proc.start([py, "-c", "import sys; sys.exit(7)"], "", env))
    let rc2 = unwrap(proc.wait(ch2, 5000))
    if rc2 != 7 { die("expected exit 7") }

    # Kill hanging child (and idempotent second kill)
    let ch3 = unwrap(proc.start([py, "-c", "import time; time.sleep(30)"], "", env))
    unwrap(proc.kill(ch3))
    unwrap(proc.kill(ch3))
    let _rc3 = unwrap(proc.wait(ch3, 5000))

    # Timeout wait kills hanging child and returns Err
    let ch4 = unwrap(proc.start([py, "-c", "import time; time.sleep(30)"], "", env))
    let w4 = proc.wait(ch4, 100)
    if !is_err(w4) { die("wait should have timed out") }

    print("PROCESS_OK")
}
'''


def build_clone_child_fail_program(python_exe: str) -> str:
    py_escaped = python_exe.replace("\\", "\\\\")
    return r'''import std.process as proc

fn main() {
    var env = map[string]string{}
    let ch = unwrap(proc.start(["''' + py_escaped + r'''", "-c", "pass"], "", env))
    let _c = clone(ch)
    let _r = proc.wait(ch, 5000)
}
'''


def build_csv_regex_sqlite_e2e_program(db_path: str) -> str:
    db_escaped = db_path.replace("\\", "\\\\")
    return r'''import sqlite as db
import std.buffer as buffer
import std.bytes as bytes
import std.csv as csv
import std.regex as regex
import std.stream as stream

fn die(msg string) {
    print("FAIL:", msg)
    let r Result<int, string> = err(msg)
    let _ = unwrap(r)
}

fn main() {
    let raw = "name,email,note\r\n\"Alice, A.\",alice@example.com,\"Line 1\nLine 2\"\r\nBob,not-an-email,\"He said \"\"hi\"\"\"\r\n张伟,zhangwei@hua.dev,clean\r\n"

    # Unclosed quote check
    var bad_r = unwrap(csv.new_reader(stream.from_bytes(bytes.from_text("\"unclosed")), ""))
    if !is_err(csv.next_row(bad_r)) {
        die("unclosed quote should fail")
    }

    # Parse streaming CSV rows
    var r = unwrap(csv.new_reader(stream.from_bytes(bytes.from_text(raw)), ""))
    let header = unwrap(unwrap(csv.next_row(r)))
    if len(header) != 3 || header[0] != "name" { die("header mismatch") }

    let email_pat = unwrap(regex.compile("^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}$"))
    let conn = unwrap(db.open("''' + db_escaped + r'''"))
    unwrap(db.exec(conn, "CREATE TABLE contacts (name TEXT, email TEXT, note TEXT)"))

    var row_idx = 0
    while true {
        let maybe_row = unwrap(csv.next_row(r))
        if is_none(maybe_row) { break }
        let row = unwrap(maybe_row)
        row_idx = row_idx + 1
        if row_idx == 1 {
            if row[0] != "Alice, A." || row[2] != "Line 1\nLine 2" {
                die("row 1 quote/newline mismatch")
            }
        }
        if row_idx == 2 {
            if row[2] != "He said \"hi\"" {
                die("row 2 escaped quote mismatch")
            }
        }
        if row_idx == 3 {
            if row[0] != "张伟" { die("row 3 utf8 mismatch") }
        }
        if regex.is_match(email_pat, row[1]) {
            unwrap(db.query_exec(conn, "INSERT INTO contacts(name, email, note) VALUES(?, ?, ?)", [
                db.Param.Text(row[0]),
                db.Param.Text(row[1]),
                db.Param.Text(row[2])
            ]))
        }
    }
    unwrap(csv.close_reader(r))
    if row_idx != 3 { die("expected 3 data rows") }

    # Query back from SQLite and write CSV via std.stream.Writer
    var out_buf = buffer.new()
    let w = stream.to_buffer(out_buf)
    let st = unwrap(db.prepare(conn, "SELECT name, email FROM contacts ORDER BY email"))
    var count = 0
    while unwrap(db.step(st)) {
        let n = unwrap(db.column_text(st, 0))
        let em = unwrap(db.column_text(st, 1))
        unwrap(csv.write_row(w, [n, em], ""))
        count = count + 1
    }
    unwrap(db.finalize(st))
    unwrap(db.close(conn))
    unwrap(stream.close_writer(w))

    if count != 2 { die("expected 2 valid contacts") }
    let formatted = unwrap(buffer.text(out_buf))
    if formatted != "\"Alice, A.\",alice@example.com\n张伟,zhangwei@hua.dev\n" {
        die("formatted csv mismatch: " + formatted)
    }

    print("CSV_REGEX_SQLITE_OK")
}
'''


def test_host_only(hua_exe: Path) -> None:
    print(f"=== Host-only pre-check ({hua_exe}) ===")
    with tempfile.TemporaryDirectory(prefix="hua_step5_host_") as tmp:
        tmp_dir = Path(tmp)
        prog = tmp_dir / "regex_proc.hua"
        prog.write_text(build_regex_and_process_program(sys.executable), encoding="utf-8")
        out = assert_ok(run_cmd([hua_exe, "run", prog], cwd=ROOT), "host regex+process")
        assert "REGEX_OK" in out and "PROCESS_OK" in out, f"unexpected output:\n{out}"
        print("  [PASS] 5B std.regex (incl. ReDoS + \\D/\\W) + 5C std.process.spawn")

        clone_prog = tmp_dir / "clone_child.hua"
        clone_prog.write_text(build_clone_child_fail_program(sys.executable), encoding="utf-8")
        proc = run_cmd([hua_exe, "run", clone_prog], cwd=ROOT)
        combined = proc.stdout + proc.stderr
        assert proc.returncode != 0 and "E3008" in combined, f"expected E3008, got rc={proc.returncode}\n{combined}"
        print("  [PASS] 5C clone(Child) -> E3008")


def test_sdk_4paths(sdk_dir: Path) -> None:
    hua_exe = sdk_dir / "bin" / "hua.exe"
    archive_exe = sdk_dir / "lib" / "compiler" / "hua-archive.exe"
    assert hua_exe.exists(), f"missing {hua_exe}"
    assert archive_exe.exists(), f"missing {archive_exe}"
    print(f"=== Full 4-path Step 5 Wave 1 E2E ({sdk_dir}) ===")

    import struct

    def huab_format(path: Path) -> int:
        magic, fmt, _abi, _size, _crc, flags = struct.unpack("<8sIIQII", path.read_bytes()[:32])
        assert magic == b"HUAB\r\n\x1a\n" and flags == 0
        return fmt

    with tempfile.TemporaryDirectory(prefix="hua_step5_e2e_") as tmp:
        tmp_dir = Path(tmp)
        sqlite_pkg = tmp_dir / "sqlite"
        build_sqlite_package(DEFAULT_CLANG, sqlite_pkg)

        base_env = os.environ.copy()
        base_env["HUA_HOME"] = str(sdk_dir)

        # 1. Regex + Process across all 4 paths
        rp_src = tmp_dir / "regex_proc.hua"
        rp_src.write_text(build_regex_and_process_program(sys.executable), encoding="utf-8")

        out1 = assert_ok(run_cmd([hua_exe, "run", rp_src], cwd=tmp_dir, env=base_env), "Path1 regex+proc")
        assert "REGEX_OK" in out1 and "PROCESS_OK" in out1

        rp_huab_host = tmp_dir / "rp_host.huab"
        assert_ok(run_cmd([hua_exe, "build", rp_src, "-o", rp_huab_host], cwd=tmp_dir, env=base_env), "Path2 build regex+proc")
        assert huab_format(rp_huab_host) == 32, f"expected format 32, got {huab_format(rp_huab_host)}"
        assert_ok(run_cmd([hua_exe, "check", rp_huab_host], cwd=tmp_dir, env=base_env), "Path2 check regex+proc")
        out2 = assert_ok(run_cmd([hua_exe, "run", rp_huab_host], cwd=tmp_dir, env=base_env), "Path2 run regex+proc")
        assert "REGEX_OK" in out2 and "PROCESS_OK" in out2

        rp_huab_sh = tmp_dir / "rp_selfhost.huab"
        assert_ok(run_cmd([archive_exe, rp_src, rp_huab_sh], cwd=tmp_dir, env=base_env), "Path3 build regex+proc")
        assert huab_format(rp_huab_sh) == 32, f"expected format 32, got {huab_format(rp_huab_sh)}"
        assert_ok(run_cmd([hua_exe, "check", rp_huab_sh], cwd=tmp_dir, env=base_env), "Path3 check regex+proc")
        out3 = assert_ok(run_cmd([hua_exe, "run", rp_huab_sh], cwd=tmp_dir, env=base_env), "Path3 run regex+proc")
        assert "REGEX_OK" in out3 and "PROCESS_OK" in out3

        rp_exe = tmp_dir / "rp_release.exe"
        assert_ok(run_cmd([hua_exe, "build", "--release", rp_src, "-o", rp_exe], cwd=tmp_dir, env=base_env), "Path4 build regex+proc")
        out4 = assert_ok(run_cmd([rp_exe], cwd=tmp_dir, env=base_env), "Path4 run regex+proc")
        assert "REGEX_OK" in out4 and "PROCESS_OK" in out4
        print("  [PASS] 5B std.regex + 5C std.process.start (all 4 paths, HUAB format 32)")

        # 2. E3008 non-cloneable Child
        clone_src = tmp_dir / "clone_child.hua"
        clone_src.write_text(build_clone_child_fail_program(sys.executable), encoding="utf-8")
        proc = run_cmd([hua_exe, "run", clone_src], cwd=tmp_dir, env=base_env)
        combined = proc.stdout + proc.stderr
        assert proc.returncode != 0 and "E3008" in combined, f"expected E3008, got:\n{combined}"
        print("  [PASS] 5C clone(Child) -> E3008")

        # 3. Integrated 5A CSV + 5B Regex + SQLite across all 4 paths + clean portable EXE
        import json
        app_dir = tmp_dir / "app"
        app_dir.mkdir()
        (app_dir / "hua.package.json").write_text(
            json.dumps({
                "schema": "hua.package.v1",
                "name": "step5_csv_sqlite",
                "version": "0.1.0",
                "deps": {"sqlite": "../sqlite"},
            }, indent=2) + "\n",
            encoding="utf-8",
        )
        for idx, mode in enumerate(["run", "host_huab", "selfhost_huab", "release_exe"], 1):
            db_path = tmp_dir / f"contacts_{mode}.db"
            if db_path.exists():
                db_path.unlink()
            e2e_src = app_dir / f"e2e_{mode}.hua"
            e2e_src.write_text(build_csv_regex_sqlite_e2e_program(str(db_path)), encoding="utf-8")

            if mode == "run":
                out = assert_ok(run_cmd([hua_exe, "run", e2e_src], cwd=app_dir, env=base_env), f"Path{idx} csv+regex+sqlite")
            elif mode == "host_huab":
                bdir = tmp_dir / "bundle_host"
                (bdir / "native/sqlite").mkdir(parents=True, exist_ok=True)
                shutil.copy2(sqlite_pkg / "sqlite_native.dll", bdir / "native/sqlite/sqlite_native.dll")
                shutil.copy2(sqlite_pkg / "sqlite3.dll", bdir / "native/sqlite/sqlite3.dll")
                art = bdir / "e2e_host.huab"
                assert_ok(run_cmd([hua_exe, "build", e2e_src, "-o", art], cwd=app_dir, env=base_env), f"Path{idx} build")
                assert huab_format(art) == 32
                assert_ok(run_cmd([hua_exe, "check", art], cwd=bdir, env=base_env), f"Path{idx} check")
                out = assert_ok(run_cmd([hua_exe, "run", art], cwd=bdir, env=base_env), f"Path{idx} run")
            elif mode == "selfhost_huab":
                bdir = tmp_dir / "bundle_sh"
                (bdir / "native/sqlite").mkdir(parents=True, exist_ok=True)
                shutil.copy2(sqlite_pkg / "sqlite_native.dll", bdir / "native/sqlite/sqlite_native.dll")
                shutil.copy2(sqlite_pkg / "sqlite3.dll", bdir / "native/sqlite/sqlite3.dll")
                art = bdir / "e2e_selfhost.huab"
                assert_ok(run_cmd([archive_exe, e2e_src, art], cwd=app_dir, env=base_env), f"Path{idx} build")
                assert huab_format(art) == 32
                assert_ok(run_cmd([hua_exe, "check", art], cwd=bdir, env=base_env), f"Path{idx} check")
                out = assert_ok(run_cmd([hua_exe, "run", art], cwd=bdir, env=base_env), f"Path{idx} run")
            else:
                out_dir = tmp_dir / "release_bundle"
                out_dir.mkdir(exist_ok=True)
                exe = out_dir / "e2e_release.exe"
                assert_ok(run_cmd([hua_exe, "build", "--release", e2e_src, "-o", exe], cwd=app_dir, env=base_env), f"Path{idx} build")
                portable_dir = tmp_dir / "portable_run"
                if portable_dir.exists():
                    shutil.rmtree(portable_dir)
                shutil.copytree(out_dir, portable_dir)
                clean_env = {"SYSTEMROOT": os.environ.get("SYSTEMROOT", r"C:\Windows"), "PATH": r"C:\Windows\System32"}
                out = assert_ok(run_cmd([portable_dir / "e2e_release.exe"], cwd=portable_dir, env=clean_env), f"Path{idx} clean portable run")

            assert "CSV_REGEX_SQLITE_OK" in out, f"[{mode}] unexpected output:\n{out}"
            print(f"  [PASS] Path {idx} ({mode}) 5A CSV + 5B Regex -> SQLite")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-only", action="store_true")
    parser.add_argument("--hua", type=Path, default=ROOT / "tests" / "build" / "hua.exe")
    parser.add_argument("--sdk", type=Path, default=ROOT / "huaBin")
    args = parser.parse_args()

    t0 = time.monotonic()
    if args.host_only:
        test_host_only(args.hua.resolve())
    else:
        test_sdk_4paths(args.sdk.resolve())
    print(f"ALL STEP 5 WAVE 1 CHECKS PASSED in {time.monotonic() - t0:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
