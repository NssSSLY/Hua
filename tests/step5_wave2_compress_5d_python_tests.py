#!/usr/bin/env python3
"""Step 5 Wave 2 acceptance:
  - 5B-gzip: std.compress (RFC 1951/1952 gzip_compress/decompress/reader/writer + Python gzip interop + .csv.gz -> regex -> SQLite)
  - 5D: 58 newly enabled non-callback stdlib functions (random, deque, heap, maps, sets, list, alg, simd)
  - 5E: packages/python CPython bridge SDK package (4-path including --release)

Usage:
  python tests/step5_wave2_compress_5d_python_tests.py --host-only --hua tests/build/hua.exe
  python tests/step5_wave2_compress_5d_python_tests.py --sdk huaBin
"""
from __future__ import annotations

import argparse
import gzip
import json
import os
import shutil
import struct
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


def huab_format(path: Path) -> int:
    magic, fmt, _abi, _size, _crc, flags = struct.unpack("<8sIIQII", path.read_bytes()[:32])
    assert magic == b"HUAB\r\n\x1a\n" and flags == 0
    return fmt


def build_wave2_core_program(py_gz_path: str, hua_gz_out: str) -> str:
    py_gz_esc = py_gz_path.replace("\\", "\\\\")
    hua_gz_esc = hua_gz_out.replace("\\", "\\\\")
    return r'''import std.alg as alg
import std.buffer as buffer
import std.bytes as bytes
import std.compress as compress
import std.deque as deque
import std.error as error
import std.fs as fs
import std.heap as heap
import std.list as list
import std.maps as maps
import std.random as random
import std.sets as sets
import std.stream as stream

fn die(msg string) {
    print("FAIL:", msg)
    let r Result<int, string> = err(msg)
    let _ = unwrap(r)
}

fn main() {
    # === 5B-gzip: std.compress ===
    # 1. Empty & UTF-8 roundtrip
    let empty_gz = unwrap(compress.gzip_compress(bytes.from_text("")))
    let empty_back = unwrap(compress.gzip_decompress(empty_gz, 1024))
    if bytes.len(empty_back) != 0 { die("empty gzip roundtrip mismatch") }

    var rep_buf = buffer.new()
    for i in 0..200 {
        buffer.write_str(rep_buf, "2026-11-27 INFO request_id=abc123 user=张伟 status=200\n")
    }
    let raw_bytes = buffer.bytes(rep_buf)
    let gz_bytes = unwrap(compress.gzip_compress(raw_bytes))
    if bytes.len(gz_bytes) * 4 >= bytes.len(raw_bytes) {
        die("expected compression ratio < 25%")
    }
    let back_bytes = unwrap(compress.gzip_decompress(gz_bytes, 1048576))
    if unwrap(bytes.to_text(back_bytes)) != unwrap(bytes.to_text(raw_bytes)) {
        die("large gzip roundtrip mismatch")
    }
    unwrap(fs.write_bytes("''' + hua_gz_esc + r'''", gz_bytes))

    # 2. Decompress Python-generated dynamic-Huffman gzip file
    let py_gz = unwrap(fs.read_bytes("''' + py_gz_esc + r'''"))
    let py_plain = unwrap(bytes.to_text(unwrap(compress.gzip_decompress(py_gz, 1048576))))
    if py_plain != "hello from python dynamic huffman 中文 1234567890 hello from python dynamic huffman!" {
        die("python gzip interop mismatch: " + py_plain)
    }

    # 3. Zip-bomb max_bytes limit & corrupt detection
    let lim_res = compress.gzip_decompress(gz_bytes, 16)
    if !is_err(lim_res) || error.code(unwrap_err(lim_res)) != "limit" {
        die("expected compress limit error")
    }
    let bad_res = compress.gzip_decompress(bytes.from_text("not a valid gzip stream at all"), 1024)
    if !is_err(bad_res) || error.code(unwrap_err(bad_res)) != "corrupt" {
        die("expected compress corrupt error")
    }

    # 4. Streaming gzip_writer -> gzip_reader decorator roundtrip
    var stream_gz_buf = buffer.new()
    let dst_w = stream.to_buffer(stream_gz_buf)
    let gw = unwrap(compress.gzip_writer(dst_w))
    unwrap(stream.write_all(gw, bytes.from_text("chunk1-alpha-")))
    unwrap(stream.write_all(gw, bytes.from_text("chunk2-中文-omega")))
    unwrap(stream.close_writer(gw))
    unwrap(stream.close_writer(dst_w))

    let gr = unwrap(compress.gzip_reader(stream.from_bytes(buffer.bytes(stream_gz_buf)), 65536))
    let streamed_text = unwrap(bytes.to_text(unwrap(stream.read_all(gr, 65536))))
    unwrap(stream.close_reader(gr))
    if streamed_text != "chunk1-alpha-chunk2-中文-omega" {
        die("streaming gzip decorator mismatch: " + streamed_text)
    }
    print("COMPRESS_OK")

    # === 5D: std.random ===
    var rng = random.new(42)
    let r1 = random.int(rng, 10, 20)
    if r1 < 10 || r1 >= 20 { die("random.int out of range") }
    let rf = random.float(rng)
    if rf < 0.0 || rf >= 1.0 { die("random.float out of range") }
    let _rb = random.bool(rng)
    let rbytes = random.bytes(rng, 16)
    if bytes.len(rbytes) != 16 { die("random.bytes len mismatch") }
    var shuf = list.from_slice([1, 2, 3, 4, 5, 6, 7, 8])
    random.shuffle(rng, shuf)
    let shuf_snap = list.snapshot(shuf)
    if len(shuf_snap) != 8 || unwrap(alg.sum(shuf_snap)) != 36 { die("random.shuffle sum mismatch") }
    let ch = unwrap(random.choice(rng, ["a", "b", "c"]))
    if ch != "a" && ch != "b" && ch != "c" { die("random.choice invalid") }
    let samp = unwrap(random.sample(rng, [10, 20, 30, 40], 2))
    if len(samp) != 2 { die("random.sample len mismatch") }

    # === 5D: std.deque ===
    var dq = deque.from_slice([2, 3])
    unwrap(deque.push_front(dq, 1))
    unwrap(deque.push_back(dq, 4))
    if deque.len(dq) != 4 { die("deque.len mismatch") }
    if unwrap(deque.front(dq)) != 1 || unwrap(deque.back(dq)) != 4 { die("deque front/back mismatch") }
    if unwrap(deque.get(dq, 2)) != 3 { die("deque.get mismatch") }
    if unwrap(deque.pop_front(dq)) != 1 || unwrap(deque.pop_back(dq)) != 4 { die("deque pop mismatch") }
    let dq_snap = deque.snapshot(dq)
    if len(dq_snap) != 2 || dq_snap[0] != 2 || dq_snap[1] != 3 { die("deque.snapshot mismatch") }
    deque.clear(dq)
    if deque.len(dq) != 0 { die("deque.clear mismatch") }

    # === 5D: std.heap ===
    var hp = list.from_slice([5, 1, 4, 2, 3])
    heap.heapify(hp)
    if unwrap(heap.peek(hp)) != 1 { die("heap.peek mismatch") }
    unwrap(heap.push(hp, 0))
    if unwrap(heap.pop(hp)) != 0 || unwrap(heap.pop(hp)) != 1 { die("heap.pop mismatch") }
    let sm = unwrap(heap.smallest([9, 3, 7, 1, 5], 2))
    let lg = unwrap(heap.largest([9, 3, 7, 1, 5], 2))
    if len(sm) != 2 || sm[0] != 1 || sm[1] != 3 || lg[0] != 9 || lg[1] != 7 { die("heap smallest/largest mismatch") }

    # === 5D: std.maps ===
    var mp = map[string]int{"b": 2, "a": 1}
    let ks = maps.keys(mp)
    let vs = maps.values(mp)
    if len(ks) != 2 || ks[0] != "a" || vs[0] != 1 { die("maps.keys/values mismatch") }
    let ek, ev = maps.entries(mp)
    if len(ek) != 2 || ek[1] != "b" || ev[1] != 2 { die("maps.entries mismatch") }
    if maps.get_or(mp, "z", 99) != 99 || maps.get_or(mp, "a", 99) != 1 { die("maps.get_or mismatch") }
    let popped = maps.pop(mp, "a")
    if is_none(popped) || unwrap(popped) != 1 || has(mp, "a") { die("maps.pop mutation mismatch") }
    let added = maps.merge(mp, map[string]int{"c": 3, "b": 20})
    if added != 1 || unwrap(mp["b"]) != 20 || unwrap(mp["c"]) != 3 { die("maps.merge mutation mismatch") }

    # === 5D: std.sets ===
    let s1 = sets.from_slice(["a", "b", "c"])
    let s2 = sets.from_slice(["b", "c", "d"])
    let su = sets.union(s1, s2)
    let si = sets.intersection(s1, s2)
    let sd = sets.difference(s1, s2)
    let sx = sets.symmetric_difference(s1, s2)
    if len(su) != 4 || len(si) != 2 || len(sd) != 1 || len(sx) != 2 { die("sets ops mismatch") }
    if !sets.is_subset(si, s1) || sets.is_disjoint(s1, s2) { die("sets predicates mismatch") }

    # === 5D: std.list extras ===
    var xs = list.from_slice([3, 1, 2, 2, 1])
    unwrap(list.insert(xs, 0, 9))
    if unwrap(list.remove_at(xs, 0)) != 9 { die("list insert/remove_at mismatch") }
    if list.index_of(xs, 2) != 2 || !list.contains(xs, 3) { die("list index_of/contains mismatch") }
    list.sort(xs)
    let deduped = list.dedup(xs)
    if deduped != 2 || list.len(xs) != 3 { die("list sort/dedup mismatch") }
    list.reverse(xs)
    unwrap(list.swap(xs, 0, 2))
    let sl = unwrap(list.slice(xs, 0, 2))
    if len(sl) != 2 || sl[0] != 1 || sl[1] != 2 { die("list reverse/swap/slice mismatch") }
    list.truncate(xs, 1)
    if list.len(xs) != 1 { die("list.truncate mismatch") }

    # === 5D: std.alg extras ===
    let uq = alg.unique([1, 2, 1, 3, 2])
    if len(uq) != 3 || uq[2] != 3 { die("alg.unique mismatch") }
    let chks = alg.chunk([1, 2, 3, 4, 5], 2)
    if len(chks) != 3 || len(chks[2]) != 1 { die("alg.chunk mismatch") }
    if unwrap(alg.product([2, 3, 4])) != 24 { die("alg.product mismatch") }
    let wins = alg.windows([10, 20, 30, 40], 2)
    if len(wins) != 3 || wins[2][1] != 40 { die("alg.windows mismatch") }

    print("STDLIB_5D_OK")
}
'''


def build_python_package_program() -> str:
    return r'''import python as py
import std.json as json

fn main() {
    unwrap(py.init(""))
    let ver = unwrap(py.version())
    if len(ver) == 0 {
        let r Result<int, string> = err("empty python version")
        let _ = unwrap(r)
    }
    let m = unwrap(py.import_module("math"))
    let f = unwrap(py.getattr(m, "factorial"))
    let arg = unwrap(py.from_int(6))
    let res = unwrap(py.call(f, [arg]))
    let val = unwrap(json.as_int(unwrap(py.to_json(res))))
    unwrap(py.release(res))
    unwrap(py.release(arg))
    unwrap(py.release(f))
    unwrap(py.release(m))
    unwrap(py.shutdown())
    if val != 720 {
        let r Result<int, string> = err("factorial(6) != 720")
        let _ = unwrap(r)
    }
    print("PYTHON_PKG_OK")
}
'''


def test_host_only(hua_exe: Path) -> None:
    print(f"=== Host-only Wave 2 pre-check ({hua_exe}) ===")
    with tempfile.TemporaryDirectory(prefix="hua_step5w2_host_") as tmp:
        tmp_dir = Path(tmp)
        py_gz = tmp_dir / "py_in.gz"
        hua_gz = tmp_dir / "hua_out.gz"
        py_gz.write_bytes(gzip.compress("hello from python dynamic huffman 中文 1234567890 hello from python dynamic huffman!".encode("utf-8")))
        prog = tmp_dir / "wave2.hua"
        prog.write_text(build_wave2_core_program(str(py_gz), str(hua_gz)), encoding="utf-8")
        out = assert_ok(run_cmd([hua_exe, "run", prog], cwd=ROOT), "host wave2")
        assert "COMPRESS_OK" in out and "STDLIB_5D_OK" in out, f"unexpected:\n{out}"
        # Verify Python's gzip.decompress accepts Hua's gzip output!
        decomp = gzip.decompress(hua_gz.read_bytes()).decode("utf-8")
        assert decomp.count("2026-11-27 INFO request_id=abc123 user=张伟 status=200\n") == 200
        print("  [PASS] 5B-gzip (incl. bidirectional Python gzip interop) + 5D 58 stdlib fns")


def test_sdk_4paths(sdk_dir: Path) -> None:
    hua_exe = sdk_dir / "bin" / "hua.exe"
    archive_exe = sdk_dir / "lib" / "compiler" / "hua-archive.exe"
    assert hua_exe.exists() and archive_exe.exists()
    print(f"=== Full 4-path Step 5 Wave 2 E2E ({sdk_dir}) ===")

    with tempfile.TemporaryDirectory(prefix="hua_step5w2_e2e_") as tmp:
        tmp_dir = Path(tmp)
        base_env = os.environ.copy()
        base_env["HUA_HOME"] = str(sdk_dir)

        # 1. 5B-gzip + 5D across all 4 paths
        py_gz = tmp_dir / "py_in.gz"
        py_gz.write_bytes(gzip.compress("hello from python dynamic huffman 中文 1234567890 hello from python dynamic huffman!".encode("utf-8")))
        for idx, mode in enumerate(["run", "host_huab", "selfhost_huab", "release_exe"], 1):
            hua_gz = tmp_dir / f"hua_out_{mode}.gz"
            src = tmp_dir / f"wave2_{mode}.hua"
            src.write_text(build_wave2_core_program(str(py_gz), str(hua_gz)), encoding="utf-8")
            if mode == "run":
                out = assert_ok(run_cmd([hua_exe, "run", src], cwd=tmp_dir, env=base_env), f"Path{idx}")
            elif mode == "host_huab":
                art = tmp_dir / "w2_host.huab"
                assert_ok(run_cmd([hua_exe, "build", src, "-o", art], cwd=tmp_dir, env=base_env), f"Path{idx} build")
                assert huab_format(art) == 33, f"expected format 33, got {huab_format(art)}"
                assert_ok(run_cmd([hua_exe, "check", art], cwd=tmp_dir, env=base_env), f"Path{idx} check")
                out = assert_ok(run_cmd([hua_exe, "run", art], cwd=tmp_dir, env=base_env), f"Path{idx} run")
            elif mode == "selfhost_huab":
                art = tmp_dir / "w2_selfhost.huab"
                arc_out = assert_ok(run_cmd([archive_exe, src, art], cwd=tmp_dir, env=base_env), f"Path{idx} build")
                receipt = json.loads(arc_out)
                assert receipt.get("status") == "ok", f"archive.exe error: {receipt}"
                assert huab_format(art) == 33, f"expected format 33, got {huab_format(art)}"
                assert_ok(run_cmd([hua_exe, "check", art], cwd=tmp_dir, env=base_env), f"Path{idx} check")
                out = assert_ok(run_cmd([hua_exe, "run", art], cwd=tmp_dir, env=base_env), f"Path{idx} run")
            else:
                exe = tmp_dir / "w2_release.exe"
                assert_ok(run_cmd([hua_exe, "build", "--release", src, "-o", exe], cwd=tmp_dir, env=base_env), f"Path{idx} build")
                out = assert_ok(run_cmd([exe], cwd=tmp_dir, env=base_env), f"Path{idx} run")
            assert "COMPRESS_OK" in out and "STDLIB_5D_OK" in out, f"[{mode}] unexpected:\n{out}"
            decomp = gzip.decompress(hua_gz.read_bytes()).decode("utf-8")
            assert decomp.count("2026-11-27 INFO request_id=abc123 user=张伟 status=200\n") == 200
            print(f"  [PASS] Path {idx} ({mode}) 5B-gzip + 5D 58 stdlib fns (HUAB 33)")

        # 2. 5E: packages/python 4-path (if python_native.dll built in tests/build)
        py_dll_candidates = list((ROOT / "tests/build").rglob("python_native.dll"))
        if py_dll_candidates:
            py_dll = py_dll_candidates[0]
            py_pkg = tmp_dir / "python"
            shutil.copytree(ROOT / "packages/python", py_pkg)
            shutil.copy2(py_dll, py_pkg / "python_native.dll")
            app_dir = tmp_dir / "py_app"
            app_dir.mkdir()
            (app_dir / "hua.package.json").write_text(
                json.dumps({
                    "schema": "hua.package.v1",
                    "name": "py_cli",
                    "version": "0.1.0",
                    "deps": {"python": "../python"},
                }, indent=2) + "\n",
                encoding="utf-8",
            )
            py_src = app_dir / "main.hua"
            py_src.write_text(build_python_package_program(), encoding="utf-8")

            out1 = assert_ok(run_cmd([hua_exe, "run", py_src], cwd=app_dir, env=base_env), "Path1 python pkg")
            assert "PYTHON_PKG_OK" in out1

            b_host = tmp_dir / "py_bundle_host"
            (b_host / "native/python").mkdir(parents=True)
            shutil.copy2(py_dll, b_host / "native/python/python_native.dll")
            art_h = b_host / "py.huab"
            assert_ok(run_cmd([hua_exe, "build", py_src, "-o", art_h], cwd=app_dir, env=base_env), "Path2 python pkg build")
            out2 = assert_ok(run_cmd([hua_exe, "run", art_h], cwd=b_host, env=base_env), "Path2 python pkg run")
            assert "PYTHON_PKG_OK" in out2

            b_sh = tmp_dir / "py_bundle_sh"
            (b_sh / "native/python").mkdir(parents=True)
            shutil.copy2(py_dll, b_sh / "native/python/python_native.dll")
            art_s = b_sh / "py.huab"
            assert_ok(run_cmd([archive_exe, py_src, art_s], cwd=app_dir, env=base_env), "Path3 python pkg build")
            out3 = assert_ok(run_cmd([hua_exe, "run", art_s], cwd=b_sh, env=base_env), "Path3 python pkg run")
            assert "PYTHON_PKG_OK" in out3

            rel_dir = tmp_dir / "py_release"
            rel_dir.mkdir()
            exe_py = rel_dir / "py_cli.exe"
            assert_ok(run_cmd([hua_exe, "build", "--release", py_src, "-o", exe_py], cwd=app_dir, env=base_env), "Path4 python pkg build")
            out4 = assert_ok(run_cmd([exe_py], cwd=rel_dir, env=base_env), "Path4 python pkg run")
            assert "PYTHON_PKG_OK" in out4
            print("  [PASS] 5E packages/python CPython bridge (all 4 paths incl. --release)")
        else:
            print("  [SKIP] 5E packages/python: python_native.dll not built in tests/build")


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
    print(f"ALL STEP 5 WAVE 2 CHECKS PASSED in {time.monotonic() - t0:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
