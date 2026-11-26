#!/usr/bin/env python3
"""Phase 3A verification: Native ABI v2 (Bytes, generational NativeHandle, std.error.Value, multi-package .huam, HUAB 30)."""

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

TEST_DLL_C = r'''
#include "hua/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_CONN 1u
#define TAG_STMT 2u

typedef struct {
    int id;
    char log_path[512];
} DemoObj;

static void log_event(const char *path, const char *msg) {
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "ab");
    if (f) {
        fputs(msg, f);
        fputc('\n', f);
        fclose(f);
    }
}

static void conn_dtor(void *p) {
    DemoObj *o = (DemoObj *)p;
    if (!o) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "dtor:conn:%d", o->id);
    log_event(o->log_path, buf);
    free(o);
}

static void stmt_dtor(void *p) {
    DemoObj *o = (DemoObj *)p;
    if (!o) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "dtor:stmt:%d", o->id);
    log_event(o->log_path, buf);
    free(o);
}

static int obj_closer(void *p, char *err_buf, size_t err_cap) {
    DemoObj *o = (DemoObj *)p;
    if (!o) return 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "close:%d", o->id);
    log_event(o->log_path, buf);
    free(o);
    (void)err_buf; (void)err_cap;
    return 0;
}

static hua_value fn_echo_bytes(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    const uint8_t *ptr = NULL;
    size_t len = 0;
    if (argc != 1 || !api->as_bytes(env, args[0], &ptr, &len)) return NULL;
    /* Copy into stack/heap temp buffer and mutate/free after api->bytes to prove host copies immediately. */
    uint8_t *tmp = (uint8_t *)malloc(len + 1);
    if (len > 0) memcpy(tmp, ptr, len);
    hua_value out = api->bytes(env, tmp, len);
    if (len > 0) memset(tmp, 0xAA, len);
    free(tmp);
    return out;
}

static hua_value fn_open_conn(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    int64_t id = 0;
    const char *lp = NULL;
    size_t lp_len = 0;
    if (argc != 2 || !api->as_int(env, args[0], &id) || !api->as_string(env, args[1], &lp, &lp_len)) return NULL;
    if (id < 0) {
        return api->err_value(env, "demo", "constraint", "negative id rejected", "demo_c", 1, 2067);
    }
    DemoObj *o = (DemoObj *)calloc(1, sizeof(DemoObj));
    o->id = (int)id;
    if (lp_len < sizeof(o->log_path)) memcpy(o->log_path, lp, lp_len);
    hua_value h = api->handle_new(env, TAG_CONN, o, conn_dtor);
    if (!h) { free(o); return NULL; }
    return api->ok_value(env, h);
}

static hua_value fn_open_stmt(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    void *raw = NULL;
    int64_t sid = 0;
    if (argc != 2 || !api->as_int(env, args[1], &sid)) return NULL;
    int st = api->handle_get(env, args[0], TAG_CONN, &raw);
    if (st == HUA_HANDLE_CLOSED) return api->err_value(env, "demo", "closed", "connection closed", NULL, 0, 0);
    if (st == HUA_HANDLE_POISONED) return api->err_value(env, "demo", "poisoned", "connection poisoned", NULL, 0, 0);
    if (st != HUA_HANDLE_OK) return NULL;
    DemoObj *conn = (DemoObj *)raw;
    DemoObj *s = (DemoObj *)calloc(1, sizeof(DemoObj));
    s->id = (int)sid;
    memcpy(s->log_path, conn->log_path, sizeof(s->log_path));
    hua_value h = api->handle_new(env, TAG_STMT, s, stmt_dtor);
    if (!h) { free(s); return NULL; }
    return api->ok_value(env, h);
}

static hua_value fn_conn_id(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    void *raw = NULL;
    if (argc != 1) return NULL;
    int st = api->handle_get(env, args[0], TAG_CONN, &raw);
    if (st == HUA_HANDLE_CLOSED) return api->err_value(env, "demo", "closed", "connection closed", NULL, 0, 0);
    if (st == HUA_HANDLE_POISONED) return api->err_value(env, "demo", "poisoned", "connection poisoned", NULL, 0, 0);
    if (st != HUA_HANDLE_OK) return NULL;
    DemoObj *conn = (DemoObj *)raw;
    return api->ok_value(env, api->integer(env, conn->id));
}

static hua_value fn_close_conn(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    char err_buf[128];
    if (argc != 1) return NULL;
    int st = api->handle_close(env, args[0], TAG_CONN, obj_closer, err_buf, sizeof(err_buf));
    if (st == HUA_HANDLE_POISONED) return api->err_value(env, "demo", "poisoned", err_buf, NULL, 0, 0);
    if (st != HUA_HANDLE_OK) return NULL;
    return api->ok_void(env);
}

static const char *const P_BYTES[] = { "Bytes" };
static const char *const P_OPEN_CONN[] = { "int", "string" };
static const char *const P_OPEN_STMT[] = { "NativeHandle", "int" };
static const char *const P_HANDLE[] = { "NativeHandle" };

static const hua_export_v2 EXPORTS[] = {
    { "echo_bytes", 1, P_BYTES, "Bytes", fn_echo_bytes },
    { "open_conn", 2, P_OPEN_CONN, "Result<NativeHandle,std.error.Value>", fn_open_conn },
    { "open_stmt", 2, P_OPEN_STMT, "Result<NativeHandle,std.error.Value>", fn_open_stmt },
    { "conn_id", 1, P_HANDLE, "Result<int,std.error.Value>", fn_conn_id },
    { "close_conn", 1, P_HANDLE, "Result<void,std.error.Value>", fn_close_conn },
};

static const hua_module_v2 MODULE = {
    HUA_NATIVE_ABI_V2,
    (uint32_t)sizeof(hua_module_v2),
    (uint32_t)(sizeof(EXPORTS) / sizeof(EXPORTS[0])),
    EXPORTS
};

HUA_MODULE_EXPORT const hua_module_v2 *hua_module_entry_v2(void) {
    return &MODULE;
}
'''

DEMO_HUAM = """HUA_NATIVE 2
pub fn echo_bytes(data Bytes) Bytes {}
pub fn open_conn(id int, log_path string) Result<NativeHandle, std.error.Value> {}
pub fn open_stmt(conn NativeHandle, sid int) Result<NativeHandle, std.error.Value> {}
pub fn conn_id(conn NativeHandle) Result<int, std.error.Value> {}
pub fn close_conn(conn NativeHandle) Result<void, std.error.Value> {}
"""


def run(argv: list[str | Path], cwd: Path, timeout: int = 60) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in argv],
        cwd=str(cwd),
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
    )


def compile_dll(clang: Path, c_src: Path, out_dll: Path) -> None:
    p = run([clang, "-shared", "-O2", "-I", ROOT / "include", c_src, "-o", out_dll], out_dll.parent)
    assert p.returncode == 0, f"DLL build failed: {p.stderr}"


def huab_header(path: Path) -> tuple[int, int]:
    magic, fmt, abi, size, crc, flags = struct.unpack("<8sIIQII", path.read_bytes()[:32])
    assert magic == b"HUAB\r\n\x1a\n" and flags == 0
    return fmt, abi


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hua", default="tests/build/hua.exe")
    parser.add_argument("--clang", default=str(DEFAULT_CLANG))
    args = parser.parse_args()
    hua = Path(args.hua).resolve()
    clang = Path(args.clang).resolve()

    with tempfile.TemporaryDirectory(prefix="hua-abiv2-") as tmp:
        root = Path(tmp)
        extpkg = root / "extpkg"
        app = root / "app"
        extpkg.mkdir()
        app.mkdir()

        # Build demo_native.dll inside sibling package extpkg
        c_file = extpkg / "demo_native.c"
        c_file.write_text(TEST_DLL_C, encoding="utf-8")
        dll_file = extpkg / "demo_native.dll"
        compile_dll(clang, c_file, dll_file)
        (extpkg / "demo_native.huam").write_text(DEMO_HUAM, encoding="utf-8")
        (extpkg / "hua.package.json").write_text(
            json.dumps({
                "schema": "hua.package.v1",
                "name": "extpkg",
                "version": "0.1.0",
                "native_libs": {"windows-x86_64": ["demo_native.dll"]},
            }, indent=2) + "\n",
            encoding="utf-8",
        )
        (extpkg / "package.hua").write_text(
            """import demo_native as raw
pub fn echo(b Bytes) Bytes { return raw.echo_bytes(b) }
pub fn open_conn(id int, log_path string) Result<NativeHandle, std.error.Value> { return raw.open_conn(id, log_path) }
pub fn open_stmt(c NativeHandle, sid int) Result<NativeHandle, std.error.Value> { return raw.open_stmt(c, sid) }
pub fn conn_id(c NativeHandle) Result<int, std.error.Value> { return raw.conn_id(c) }
pub fn close_conn(c NativeHandle) Result<void, std.error.Value> { return raw.close_conn(c) }
""",
            encoding="utf-8",
        )

        # App depends on ../extpkg
        (app / "hua.package.json").write_text(
            json.dumps({
                "schema": "hua.package.v1",
                "name": "app",
                "version": "0.1.0",
                "deps": {"extpkg": "../extpkg"},
            }, indent=2) + "\n",
            encoding="utf-8",
        )
        log_file = root / "lifecycle.log"
        (app / "main.hua").write_text(
            """import extpkg as ext
import std.bytes as bytes
import std.error as err
import std.os as os

fn main() {
    let argv = os.args()
    let log_path = argv[0]

    # 1. Bytes with embedded NUL and non-UTF-8 bytes
    let raw = unwrap(bytes.from_ints([0, 255, 1, 0, 254, 65]))
    let echoed = ext.echo(raw)
    print("bytes:", bytes.len(echoed), unwrap(bytes.at(echoed, 0)), unwrap(bytes.at(echoed, 1)), unwrap(bytes.at(echoed, 3)), unwrap(bytes.at(echoed, 4)))

    # 2. Recoverable structured error
    let bad = ext.open_conn(-5, log_path)
    let e = unwrap_err(bad)
    print("err:", err.domain(e), err.code(e), err.message(e), unwrap(err.native_domain(e)), unwrap(err.native_code(e)))

    # 3. Handle lifecycle: open, use, close, repeat close (idempotent), use-after-close -> code=="closed"
    let c1 = unwrap(ext.open_conn(10, log_path))
    print("c1:", unwrap(ext.conn_id(c1)))
    unwrap(ext.close_conn(c1))
    unwrap(ext.close_conn(c1))
    let after = ext.conn_id(c1)
    print("after_close:", err.domain(unwrap_err(after)), err.code(unwrap_err(after)))

    # 4. Unclosed handles destroyed in LIFO order (stmt 99 before conn 20)
    let c2 = unwrap(ext.open_conn(20, log_path))
    let _ = unwrap(ext.open_stmt(c2, 99))
    print("done: ok")
}
""",
            encoding="utf-8",
        )

        # Run from source (VM)
        p_vm = run([hua, "run", app / "main.hua", "--", str(log_file)], app)
        assert p_vm.returncode == 0, f"VM failed: {p_vm.stderr}"
        expected_out = (
            "bytes: 6 0 255 0 254\n"
            "err: demo constraint negative id rejected demo_c 2067\n"
            "c1: 10\n"
            "after_close: demo closed\n"
            "done: ok\n"
        )
        assert p_vm.stdout == expected_out, f"VM stdout mismatch:\n{p_vm.stdout}"
        events = log_file.read_text(encoding="utf-8").splitlines()
        assert events == ["close:10", "dtor:stmt:99", "dtor:conn:20"], f"Unexpected LIFO cleanup: {events}"
        print("[PASS] Source VM ABI v2 Bytes + Error + Handle lifecycle + LIFO cleanup", flush=True)

        # Build host HUAB -> verify format 30, native_abi 2 -> run in isolated bundle dir
        bundle = root / "bundle"
        (bundle / "native/extpkg").mkdir(parents=True)
        shutil.copy2(dll_file, bundle / "native/extpkg/demo_native.dll")
        huab_path = bundle / "app.huab"
        p_build = run([hua, "build", app / "main.hua", "-o", huab_path], app)
        assert p_build.returncode == 0, f"HUAB build failed: {p_build.stderr}"
        fmt, abi = huab_header(huab_path)
        assert (fmt, abi) == (30, 2), f"Expected HUAB (30, 2), got {(fmt, abi)}"
        assert run([hua, "check", huab_path], bundle).returncode == 0
        log_file2 = root / "lifecycle_huab.log"
        p_huab = run([hua, "run", huab_path, "--", str(log_file2)], bundle)
        assert p_huab.returncode == 0 and p_huab.stdout == expected_out, f"HUAB run failed: {p_huab.stdout} {p_huab.stderr}"
        assert log_file2.read_text(encoding="utf-8").splitlines() == ["close:10", "dtor:stmt:99", "dtor:conn:20"]
        print("[PASS] Host HUAB (format 30, native_abi 2) isolated bundle execution", flush=True)

        # Negative 1: Missing DLL in bundle -> E7001
        (bundle / "native/extpkg/demo_native.dll").unlink()
        p_miss = run([hua, "run", huab_path, "--", str(log_file2)], bundle)
        assert p_miss.returncode != 0 and "E7001" in p_miss.stderr, f"Expected E7001: {p_miss.stderr}"

        # Negative 2: Wrong type_tag (passing Stmt to conn_id) -> E7004
        (app / "wrong_tag.hua").write_text(
            """import extpkg as ext
fn main() {
    let c = unwrap(ext.open_conn(1, ""))
    let s = unwrap(ext.open_stmt(c, 2))
    let _ = ext.conn_id(s)
}
""",
            encoding="utf-8",
        )
        p_tag = run([hua, "run", app / "wrong_tag.hua"], app)
        assert p_tag.returncode != 0 and "E7004" in p_tag.stderr, f"Expected E7004: {p_tag.stderr}"

        # Negative 3: clone(NativeHandle) -> E3008
        (app / "clone_h.hua").write_text(
            """import extpkg as ext
fn main() {
    let c = unwrap(ext.open_conn(1, ""))
    let _ = clone(c)
}
""",
            encoding="utf-8",
        )
        p_clone = run([hua, "check", app / "clone_h.hua"], app)
        assert p_clone.returncode != 0 and "E3008" in p_clone.stderr, f"Expected E3008: {p_clone.stderr}"

        # Negative 4: spawn capturing NativeHandle -> E4104
        (app / "spawn_h.hua").write_text(
            """import extpkg as ext
async fn use_h(h NativeHandle) int { return 1 }
fn main() {
    let c = unwrap(ext.open_conn(1, ""))
    let t = spawn use_h(c)
    print(await t)
}
""",
            encoding="utf-8",
        )
        p_spawn = run([hua, "run", app / "spawn_h.hua"], app)
        assert p_spawn.returncode != 0 and ("E3008" in p_spawn.stderr or "E4104" in p_spawn.stderr), f"Expected E3008/E4104: {p_spawn.stderr}"

        # Negative 5: Bad HUA_NATIVE 99 header -> E7002
        (app / "bad_hdr.huam").write_text("HUA_NATIVE 99\npub fn f() void {}\n", encoding="utf-8")
        (app / "use_bad_hdr.hua").write_text("import bad_hdr as b\nfn main() { b.f() }\n", encoding="utf-8")
        p_hdr = run([hua, "check", app / "use_bad_hdr.hua"], app)
        assert p_hdr.returncode != 0 and "E7002" in p_hdr.stderr, f"Expected E7002: {p_hdr.stderr}"

        # Negative 6: Missing native_libs file -> E7001
        bad_pkg = root / "bad_pkg"
        bad_pkg.mkdir()
        (bad_pkg / "hua.package.json").write_text(
            json.dumps({
                "schema": "hua.package.v1",
                "name": "bad_pkg",
                "version": "0.1.0",
                "native_libs": {"windows-x86_64": ["does_not_exist.dll"]},
            }),
            encoding="utf-8",
        )
        (bad_pkg / "main.hua").write_text("fn main() {}\n", encoding="utf-8")
        p_nlib = run([hua, "check", bad_pkg / "main.hua"], bad_pkg)
        assert p_nlib.returncode != 0 and "E7001" in p_nlib.stderr, f"Expected E7001: {p_nlib.stderr}"

        print("[PASS] All 6 negative ABI v2 diagnostics (E7001/E7002/E7004/E3008/E4104)", flush=True)
    print("ALL PHASE 3A HOST ABI V2 TESTS PASSED")


if __name__ == "__main__":
    main()
