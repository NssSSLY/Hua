#!/usr/bin/env python3
"""Step 3C end-to-end acceptance: SQLite native extension package across 4 paths + clean portable bundle."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CLANG = ROOT / ".tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang.exe"


def clean_env(sdk: Path | None, extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    sys_root = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = str(Path(sys_root) / "System32")
    if sdk is not None:
        env["HUA_HOME"] = str(sdk)
    else:
        env.pop("HUA_HOME", None)
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


def huab_header(path: Path) -> tuple[int, int]:
    magic, fmt, abi, size, crc, flags = struct.unpack("<8sIIQII", path.read_bytes()[:32])
    assert magic == b"HUAB\r\n\x1a\n" and flags == 0
    return fmt, abi


def find_sqlite3_dll() -> Path:
    cand = Path(sys.base_prefix) / "DLLs" / "sqlite3.dll"
    if cand.is_file():
        return cand
    raise FileNotFoundError(f"sqlite3.dll not found at {cand}")


def build_sqlite_package(clang: Path, dst_pkg: Path) -> None:
    src_pkg = ROOT / "packages/sqlite"
    shutil.copytree(src_pkg, dst_pkg)
    shutil.copy2(find_sqlite3_dll(), dst_pkg / "sqlite3.dll")
    p = run(
        [clang, "-shared", "-O2", "-I", ROOT / "include", dst_pkg / "sqlite_native.c", "-o", dst_pkg / "sqlite_native.dll"],
        dst_pkg,
        os.environ.copy(),
    )
    assert p.returncode == 0, f"sqlite_native.dll compile failed: {p.stderr}"


APP_MAIN_HUA = r'''import sqlite as db
import std.bytes as bytes
import std.error as error
import std.os as os

fn insert_batch(conn db.Db, key string, blob Bytes, fail_dup bool) Result<void, error.Value> {
    db.begin(conn)?
    let r1 = db.query_exec(conn, "INSERT INTO items(k, label, payload) VALUES(?, ?, ?)", [
        db.Param.Text(key),
        db.Param.Text("中文记录"),
        db.Param.Blob(blob)
    ])
    if is_err(r1) {
        let _ = db.rollback(conn)
        return err(unwrap_err(r1))
    }
    if fail_dup {
        let r2 = db.query_exec(conn, "INSERT INTO items(k, label, payload) VALUES(?, ?, ?)", [
            db.Param.Text(key),
            db.Param.Text("duplicate"),
            db.Param.Null()
        ])
        if is_err(r2) {
            let _ = db.rollback(conn)
            return err(unwrap_err(r2))
        }
    }
    db.commit(conn)?
    return ok()
}

fn count_rows(conn db.Db) Result<int, error.Value> {
    let st = db.prepare(conn, "SELECT COUNT(*) FROM items")?
    defer db.finalize(st)
    let _ = db.step(st)?
    return db.column_int(st, 0)
}

fn main() {
    let argv = os.args()
    let db_path = argv[0]
    let conn = unwrap(db.open(db_path))

    unwrap(db.exec(conn, "CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, k TEXT UNIQUE NOT NULL, label TEXT, payload BLOB)"))

    # 1. Commit transaction with embedded NUL + non-UTF-8 BLOB
    let payload = unwrap(bytes.from_ints([0, 255, 1, 0, 128, 0, 65]))
    unwrap(insert_batch(conn, "k1", payload, false))

    # 2. Failed transaction (UNIQUE constraint) must roll back cleanly
    let dup_res = insert_batch(conn, "k2", payload, true)
    let dup_err = unwrap_err(dup_res)
    print("rollback_err:", error.domain(dup_err), error.code(dup_err), unwrap(error.native_domain(dup_err)), unwrap(error.native_code(dup_err)) > 0)
    print("row_count:", unwrap(count_rows(conn)))

    # 3. Read back record and verify every byte of BLOB
    let q = unwrap(db.prepare(conn, "SELECT k, label, payload FROM items WHERE k = ?"))
    unwrap(db.bind(q, 1, db.Param.Text("k1")))
    let has_row = unwrap(db.step(q))
    let k = unwrap(db.column_text(q, 0))
    let label = unwrap(db.column_text(q, 1))
    let got_blob = unwrap(db.column_blob(q, 2))
    let blob_type = unwrap(db.column_type(q, 2))
    unwrap(db.finalize(q))
    unwrap(db.finalize(q))
    print("row:", has_row, k, label, blob_type, bytes.len(got_blob), unwrap(bytes.at(got_blob, 0)), unwrap(bytes.at(got_blob, 1)), unwrap(bytes.at(got_blob, 3)), unwrap(bytes.at(got_blob, 5)), unwrap(bytes.at(got_blob, 6)))

    # 4. Close + idempotent repeat close + use-after-close -> code=="closed"
    unwrap(db.close(conn))
    unwrap(db.close(conn))
    let after = db.exec(conn, "SELECT 1")
    print("after_close:", error.domain(unwrap_err(after)), error.code(unwrap_err(after)))
}
'''


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", default="huaBin")
    parser.add_argument("--clang", default=str(DEFAULT_CLANG))
    args = parser.parse_args()
    sdk = Path(args.sdk).resolve()
    clang = Path(args.clang).resolve()
    hua = sdk / "bin/hua.exe"
    archive = sdk / "lib/compiler/hua-archive.exe"

    with tempfile.TemporaryDirectory(prefix="hua-sqlite-e2e-") as tmp:
        root = Path(tmp)
        sqlite_pkg = root / "sqlite"
        app = root / "app"
        cache_dir = root / "cache"
        build_sqlite_package(clang, sqlite_pkg)
        app.mkdir()

        (app / "hua.package.json").write_text(
            json.dumps({
                "schema": "hua.package.v1",
                "name": "sqlite_cli",
                "version": "0.1.0",
                "deps": {"sqlite": "../sqlite"},
            }, indent=2) + "\n",
            encoding="utf-8",
        )
        (app / "main.hua").write_text(APP_MAIN_HUA, encoding="utf-8")

        env = clean_env(sdk, {"HUA_RELEASE_CACHE": str(cache_dir)})
        expected = (
            "rollback_err: sqlite constraint sqlite3 true\n"
            "row_count: 1\n"
            "row: true k1 中文记录 blob 7 0 255 0 0 65\n"
            "after_close: sqlite closed\n"
        )

        # Path 1: VM from source
        db1 = root / "p1.db"
        p1 = run([hua, "run", app / "main.hua", "--", str(db1)], app, env)
        assert p1.returncode == 0 and p1.stdout == expected, f"VM failed (rc={p1.returncode}):\nSTDOUT:\n{p1.stdout}\nSTDERR:\n{p1.stderr}"
        print("[PASS] Path 1: hua run (source VM)", flush=True)

        # Path 2: Host HUAB in isolated bundle dir
        b_host = root / "bundle_host"
        (b_host / "native/sqlite").mkdir(parents=True)
        shutil.copy2(sqlite_pkg / "sqlite_native.dll", b_host / "native/sqlite/sqlite_native.dll")
        shutil.copy2(sqlite_pkg / "sqlite3.dll", b_host / "native/sqlite/sqlite3.dll")
        host_huab = b_host / "app.huab"
        pb_host = run([hua, "build", app / "main.hua", "-o", host_huab], app, env)
        assert pb_host.returncode == 0, f"Host HUAB build failed: {pb_host.stderr}"
        assert huab_header(host_huab) == (30, 2)
        assert run([hua, "check", host_huab], b_host, env).returncode == 0
        db2 = root / "p2.db"
        p2 = run([hua, "run", host_huab, "--", str(db2)], b_host, env)
        assert p2.returncode == 0 and p2.stdout == expected, f"Host HUAB failed: {p2.stdout}\n{p2.stderr}"
        print("[PASS] Path 2: Host HUAB (format 30, native_abi 2)", flush=True)

        # Path 3: Selfhost HUAB in isolated bundle dir
        b_sh = root / "bundle_sh"
        (b_sh / "native/sqlite").mkdir(parents=True)
        shutil.copy2(sqlite_pkg / "sqlite_native.dll", b_sh / "native/sqlite/sqlite_native.dll")
        shutil.copy2(sqlite_pkg / "sqlite3.dll", b_sh / "native/sqlite/sqlite3.dll")
        sh_huab = b_sh / "app.huab"
        pb_sh = run([archive, app / "main.hua", sh_huab], app, env)
        assert pb_sh.returncode == 0, f"Selfhost archive failed: {pb_sh.stdout}\n{pb_sh.stderr}"
        sh_receipt = json.loads(pb_sh.stdout)
        assert sh_receipt.get("status") == "ok" and sh_receipt.get("format") == 30, sh_receipt
        assert huab_header(sh_huab) == (30, 2)
        pc_sh = run([hua, "check", sh_huab], b_sh, env)
        assert pc_sh.returncode == 0, f"Selfhost HUAB check failed: {pc_sh.stdout}\n{pc_sh.stderr}"
        db3 = root / "p3.db"
        p3 = run([hua, "run", sh_huab, "--", str(db3)], b_sh, env)
        assert p3.returncode == 0 and p3.stdout == expected, f"Selfhost HUAB failed: {p3.stdout}\n{p3.stderr}"
        print("[PASS] Path 3: Selfhost HUAB (format 30, native_abi 2)", flush=True)

        # Path 4: hua build --release -> portable bundle copied to completely clean directory
        dist = root / "dist"
        dist.mkdir()
        exe_out = dist / "sqlite_app.exe"
        pb_rel = run([hua, "build", "--release", app / "main.hua", "-o", exe_out], app, env)
        assert pb_rel.returncode == 0, f"Release build failed: {pb_rel.stdout}\n{pb_rel.stderr}"
        rel_receipt = json.loads(pb_rel.stdout)
        assert "native/sqlite/sqlite_native.dll" in rel_receipt.get("native_artifacts", []), rel_receipt
        assert "native/sqlite/sqlite3.dll" in rel_receipt.get("native_artifacts", []), rel_receipt

        clean_dir = root / "clean_deploy"
        shutil.copytree(dist, clean_dir)
        db4 = clean_dir / "prod.db"
        p4 = run([clean_dir / "sqlite_app.exe", str(db4)], clean_dir, clean_env(None))
        assert p4.returncode == 0 and p4.stdout == expected, f"Clean EXE failed: {p4.stdout}\n{p4.stderr}"
        print("[PASS] Path 4: Release EXE portable bundle in clean environment", flush=True)

        # Path 4b: N3 cache hit into a brand-new output directory still populates native/sqlite/*.dll
        dist2 = root / "dist_cached"
        dist2.mkdir()
        pb_rel2 = run([hua, "build", "--release", app / "main.hua", "-o", dist2 / "sqlite_app.exe"], app, env)
        assert pb_rel2.returncode == 0, f"Cached release build failed: {pb_rel2.stderr}"
        rel_receipt2 = json.loads(pb_rel2.stdout)
        assert rel_receipt2.get("cache_status") == "hit", rel_receipt2
        assert (dist2 / "native/sqlite/sqlite_native.dll").is_file()
        assert (dist2 / "native/sqlite/sqlite3.dll").is_file()
        print("[PASS] Path 4b: N3 cache hit restores EXE + companion DLLs", flush=True)

        # Negative: Removing DLL from clean deploy dir produces E7001
        (clean_dir / "native/sqlite/sqlite_native.dll").unlink()
        p_miss = run([clean_dir / "sqlite_app.exe", str(clean_dir / "miss.db")], clean_dir, clean_env(None))
        assert p_miss.returncode != 0 and "E7001" in p_miss.stderr, f"Expected E7001: {p_miss.stderr}"
        print("[PASS] Negative: Missing companion DLL produces E7001", flush=True)

    print("ALL STEP 3C SQLITE PACKAGE ACCEPTANCE TESTS PASSED")


if __name__ == "__main__":
    main()
