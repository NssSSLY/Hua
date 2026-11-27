#!/usr/bin/env python3
"""Direction A (422/422 100% 4-path stdlib) acceptance suite:
  - A1: --release native async/spawn/await/taskgroup + all 9 std.task functions
  - A2: all 19 higher-order callback stdlib functions (alg 13, list 3, heap 2, maps 1)
  - A3: all 4 std.simd functions across all 4 paths

Usage:
  python tests/step5_direction_a_tests.py --sdk huaBin
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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


DIRECTION_A_HUA = r'''import std.alg as alg
import std.heap as heap
import std.list as list
import std.maps as maps
import std.simd as vec
import std.task as task

fn die(msg string) {
    print("FAIL:", msg)
    let r Result<int, string> = err(msg)
    let _ = unwrap(r)
}

# === A1: async / spawn / taskgroup / std.task (9 fns) ===
async fn add_delayed(a int, b int, ms int) int {
    task.sleep(ms)
    task.yield()
    let _wid = task.worker_id()
    return a + b
}

async fn slow_worker() int {
    for i in 0..50 {
        task.sleep(20)
    }
    return 999
}

fn test_a1_tasks() {
    # 1. async fn + await
    let t1 = add_delayed(10, 32, 10)
    if await t1 != 42 { die("async add_delayed mismatch") }

    # 2. task.all & task.race
    let a = add_delayed(1, 2, 10)
    let b = add_delayed(3, 4, 15)
    let both = await task.all(a, b)
    if len(both) != 2 || both[0] != 3 || both[1] != 7 { die("task.all mismatch") }

    let r1 = add_delayed(100, 1, 5)
    let r2 = add_delayed(200, 2, 80)
    let winner = await task.race(r1, r2)
    if winner != 101 { die("task.race mismatch") }

    # 3. task.timeout (expires -> Err)
    let slow1 = slow_worker()
    let to_res = task.timeout(slow1, 30)
    if !is_err(to_res) { die("task.timeout should expire") }

    # 4. task.cancel / done / state
    let slow2 = slow_worker()
    let _c = task.cancel(slow2)
    let _st = task.state(slow2)
    let _dn = task.done(slow2)
    let _consumed = task.timeout(slow2, 200)

    # 5. taskgroup structured concurrency
    taskgroup {
        let g1 = add_delayed(5, 6, 10)
        let g2 = add_delayed(7, 8, 10)
        if await g1 + await g2 != 26 { die("taskgroup sum mismatch") }
    }
    print("A1_TASKS_OK")
}

# === A2: 19 higher-order callback stdlib functions ===
struct Item {
    name string
    score int
}

fn dbl(x int) int { return x * 2 }
fn add2(acc int, x int) int { return acc + x }
fn is_even(x int) bool { return x % 2 == 0 }
fn gt10(x int) bool { return x > 10 }
fn item_score(it Item) int { return it.score }
fn desc_int(a int, b int) bool { return a > b }
fn mul_pair(a int, b int) int { return a * b }
fn inc_one(v int) int { return v + 1 }

fn test_a2_callbacks() {
    # alg: map, fold, filter, any, all, find, find_index, partition, zip_with
    let mapped = alg.map([1, 2, 3], dbl)
    if len(mapped) != 3 || mapped[2] != 6 { die("alg.map mismatch") }
    if alg.fold([1, 2, 3, 4], 10, add2) != 20 { die("alg.fold mismatch") }
    let filt = alg.filter([1, 2, 3, 4, 5], is_even)
    if len(filt) != 2 || filt[0] != 2 || filt[1] != 4 { die("alg.filter mismatch") }
    if !alg.any([1, 2, 3], is_even) || alg.any([1, 3, 5], is_even) { die("alg.any mismatch") }
    if !alg.all([2, 4, 6], is_even) || alg.all([2, 3, 6], is_even) { die("alg.all mismatch") }
    let found = alg.find([1, 3, 4, 6], is_even)
    if is_none(found) || unwrap(found) != 4 { die("alg.find mismatch") }
    if alg.find_index([1, 3, 4, 6], is_even) != 2 || alg.find_index([1, 3], gt10) != -1 { die("alg.find_index mismatch") }
    let evens, odds = alg.partition([1, 2, 3, 4, 5], is_even)
    if len(evens) != 2 || len(odds) != 3 || evens[1] != 4 || odds[2] != 5 { die("alg.partition mismatch") }
    let zipped = alg.zip_with([2, 3, 4], [10, 20], mul_pair)
    if len(zipped) != 2 || zipped[0] != 20 || zipped[1] != 60 { die("alg.zip_with mismatch") }

    # alg: sorted_by_key, sorted_by, min_by_key, max_by_key (incl. stability)
    let items = [Item{name: "b", score: 20}, Item{name: "a", score: 10}, Item{name: "c", score: 20}]
    let by_k = alg.sorted_by_key(items, item_score)
    if by_k[0].name != "a" || by_k[1].name != "b" || by_k[2].name != "c" { die("alg.sorted_by_key stability mismatch") }
    let by_desc = alg.sorted_by([3, 1, 4, 2], desc_int)
    if by_desc[0] != 4 || by_desc[3] != 1 { die("alg.sorted_by mismatch") }
    if unwrap(alg.min_by_key(items, item_score)).name != "a" { die("alg.min_by_key mismatch") }
    if unwrap(alg.max_by_key(items, item_score)).name != "b" { die("alg.max_by_key mismatch") }

    # list: sort_by_key, sort_by, retain
    var l_items = list.from_slice(items)
    list.sort_by_key(l_items, item_score)
    let l_snap = list.snapshot(l_items)
    if l_snap[0].name != "a" || l_snap[1].name != "b" { die("list.sort_by_key mismatch") }

    var l_nums = list.from_slice([3, 1, 4, 2, 5])
    list.sort_by(l_nums, desc_int)
    if unwrap(list.get(l_nums, 0)) != 5 || unwrap(list.get(l_nums, 4)) != 1 { die("list.sort_by mismatch") }
    let removed = list.retain(l_nums, is_even)
    if removed != 3 || list.len(l_nums) != 2 || unwrap(list.get(l_nums, 0)) != 4 { die("list.retain mismatch") }

    # heap: push_by_key, pop_by_key
    var empty_items []Item = []
    var h = list.from_slice(empty_items)
    unwrap(heap.push_by_key(h, Item{name: "mid", score: 20}, item_score))
    unwrap(heap.push_by_key(h, Item{name: "low", score: 5}, item_score))
    unwrap(heap.push_by_key(h, Item{name: "high", score: 50}, item_score))
    if unwrap(heap.pop_by_key(h, item_score)).name != "low" { die("heap.pop_by_key 1 mismatch") }
    if unwrap(heap.pop_by_key(h, item_score)).name != "mid" { die("heap.pop_by_key 2 mismatch") }

    # maps: update
    var counts = map[string]int{"x": 10}
    let v1 = maps.update(counts, "x", 0, inc_one)
    let v2 = maps.update(counts, "y", 0, inc_one)
    if v1 != 11 || v2 != 1 || unwrap(counts["x"]) != 11 || unwrap(counts["y"]) != 1 { die("maps.update mismatch") }

    print("A2_CALLBACKS_OK")
}

# === A3: std.simd (4 fns) ===
fn test_a3_simd() {
    let _be = vec.backend()
    let va = unwrap(vec.add([1.0, 2.0, 3.0], [4.0, 5.0, 6.0]))
    let vs = unwrap(vec.sub([10.0, 20.0], [3.0, 5.0]))
    let vm = unwrap(vec.mul([2.0, 3.0], [4.0, 5.0]))
    if va[2] != 9.0 || vs[1] != 15.0 || vm[1] != 15.0 { die("std.simd mismatch") }
    print("A3_SIMD_OK")
}

fn main() {
    test_a1_tasks()
    test_a2_callbacks()
    test_a3_simd()
}
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, default=ROOT / "huaBin")
    args = parser.parse_args()
    sdk_dir = args.sdk.resolve()
    hua_exe = sdk_dir / "bin" / "hua.exe"
    archive_exe = sdk_dir / "lib" / "compiler" / "hua-archive.exe"
    assert hua_exe.exists() and archive_exe.exists()

    t0 = time.monotonic()
    print(f"=== Direction A (A1 tasks + A2 19 callbacks + A3 simd) 4-path E2E ({sdk_dir}) ===")
    with tempfile.TemporaryDirectory(prefix="hua_dir_a_") as tmp:
        tmp_dir = Path(tmp)
        env = os.environ.copy()
        env["HUA_HOME"] = str(sdk_dir)
        src = tmp_dir / "dir_a.hua"
        src.write_text(DIRECTION_A_HUA, encoding="utf-8")

        # Path 1: hua run
        out1 = assert_ok(run_cmd([hua_exe, "run", src], cwd=tmp_dir, env=env), "Path1 run")
        assert "A1_TASKS_OK" in out1 and "A2_CALLBACKS_OK" in out1 and "A3_SIMD_OK" in out1
        print("  [PASS] Path 1 (hua run)")

        # Path 2: Host HUAB
        h_huab = tmp_dir / "dir_a_host.huab"
        assert_ok(run_cmd([hua_exe, "build", src, "-o", h_huab], cwd=tmp_dir, env=env), "Path2 build")
        assert_ok(run_cmd([hua_exe, "check", h_huab], cwd=tmp_dir, env=env), "Path2 check")
        out2 = assert_ok(run_cmd([hua_exe, "run", h_huab], cwd=tmp_dir, env=env), "Path2 run")
        assert "A1_TASKS_OK" in out2 and "A2_CALLBACKS_OK" in out2 and "A3_SIMD_OK" in out2
        print("  [PASS] Path 2 (Host HUAB)")

        # Path 3: Selfhost HUAB
        s_huab = tmp_dir / "dir_a_selfhost.huab"
        arc_out = assert_ok(run_cmd([archive_exe, src, s_huab], cwd=tmp_dir, env=env), "Path3 build")
        receipt = json.loads(arc_out)
        assert receipt.get("status") == "ok", f"archive.exe error: {receipt}"
        assert_ok(run_cmd([hua_exe, "check", s_huab], cwd=tmp_dir, env=env), "Path3 check")
        out3 = assert_ok(run_cmd([hua_exe, "run", s_huab], cwd=tmp_dir, env=env), "Path3 run")
        assert "A1_TASKS_OK" in out3 and "A2_CALLBACKS_OK" in out3 and "A3_SIMD_OK" in out3
        print("  [PASS] Path 3 (Selfhost HUAB)")

        # Path 4: Selfhost Native --release EXE
        exe = tmp_dir / "dir_a_release.exe"
        assert_ok(run_cmd([hua_exe, "build", "--release", src, "-o", exe], cwd=tmp_dir, env=env), "Path4 build")
        out4 = assert_ok(run_cmd([exe], cwd=tmp_dir, env=env), "Path4 run")
        assert "A1_TASKS_OK" in out4 and "A2_CALLBACKS_OK" in out4 and "A3_SIMD_OK" in out4
        print("  [PASS] Path 4 (Selfhost Native --release EXE)")

    print(f"ALL DIRECTION A CHECKS PASSED in {time.monotonic() - t0:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
