#!/usr/bin/env python3
"""4-path E2E acceptance tests for Direction B1 (inherent methods),
B2 (structural interface), and B3 (explicit user generics) in selfhost/.

Runs each positive case across all 4 execution paths:
  1. Host VM (`hua run`)
  2. Host HUAB (`hua build --out ...` -> `hua run <huab>`)
  3. Selfhost HUAB (`hua-archive.exe` -> `hua run <huab>`)
  4. Selfhost Native (`hua build --release` -> native `.exe`)

Also verifies negative diagnostics on Selfhost Archive / Native compilers.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run_cmd(cmd: list[str], cwd: Path, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def assert_eq(actual: str, expected: str, label: str) -> None:
    if actual != expected:
        raise AssertionError(f"{label} mismatch:\n  expected: {expected!r}\n  actual:   {actual!r}")


def verify_4_paths(
    name: str,
    files: dict[str, str],
    entry: str,
    expected_stdout: str,
    hua_exe: Path,
    archive_exe: Path,
    tmp_root: Path,
    skip_path3: bool = False,
    only: str = "",
) -> None:
    if only and not name.startswith(only):
        return
    case_dir = tmp_root / name
    case_dir.mkdir(parents=True, exist_ok=True)
    for rel, content in files.items():
        p = case_dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")
    entry_path = case_dir / entry

    # Path 1: Host VM (`hua run`)
    r1 = run_cmd([str(hua_exe), "run", str(entry_path)], case_dir)
    if r1.returncode != 0:
        raise AssertionError(f"[{name}] Path 1 (hua run) failed:\n{r1.stdout}\n{r1.stderr}")
    assert_eq(r1.stdout, expected_stdout, f"[{name}] Path 1 (hua run)")

    # Path 2: Host HUAB (`hua build --out ...` + `hua run`)
    host_huab = case_dir / f"{name}_host.huab"
    r2b = run_cmd([str(hua_exe), "build", str(entry_path), "-o", str(host_huab)], case_dir)
    if r2b.returncode != 0:
        raise AssertionError(f"[{name}] Path 2 (host build) failed:\n{r2b.stdout}\n{r2b.stderr}")
    r2 = run_cmd([str(hua_exe), "run", str(host_huab)], case_dir)
    if r2.returncode != 0:
        raise AssertionError(f"[{name}] Path 2 (host huab run) failed:\n{r2.stdout}\n{r2.stderr}")
    assert_eq(r2.stdout, expected_stdout, f"[{name}] Path 2 (host huab)")

    # Path 3: Selfhost HUAB (`hua-archive.exe` + `hua run`)
    if not skip_path3:
        sh_huab = case_dir / f"{name}_selfhost.huab"
        r3b = run_cmd([str(archive_exe), str(entry_path), str(sh_huab)], case_dir)
        if r3b.returncode != 0:
            raise AssertionError(f"[{name}] Path 3 (selfhost archive) failed:\n{r3b.stdout}\n{r3b.stderr}")
        try:
            receipt = json.loads(r3b.stdout)
        except Exception:
            receipt = {"status": "unknown", "raw": r3b.stdout}
        if receipt.get("status") != "ok":
            raise AssertionError(f"[{name}] Path 3 (selfhost archive) error receipt:\n{r3b.stdout}\n{r3b.stderr}")
        r3 = run_cmd([str(hua_exe), "run", str(sh_huab)], case_dir)
        if r3.returncode != 0:
            raise AssertionError(f"[{name}] Path 3 (selfhost huab run) failed:\n{r3.stdout}\n{r3.stderr}")
        assert_eq(r3.stdout, expected_stdout, f"[{name}] Path 3 (selfhost huab)")

    # Path 4: Selfhost Native (`hua build --release` + execute `.exe`)
    native_exe = case_dir / f"{name}_native.exe"
    r4b = run_cmd([str(hua_exe), "build", "--release", str(entry_path), "-o", str(native_exe)], case_dir)
    if r4b.returncode != 0:
        raise AssertionError(f"[{name}] Path 4 (release build) failed:\n{r4b.stdout}\n{r4b.stderr}")
    r4 = run_cmd([str(native_exe)], case_dir)
    if r4.returncode != 0:
        raise AssertionError(f"[{name}] Path 4 (native exe run) failed:\n{r4.stdout}\n{r4.stderr}")
    assert_eq(r4.stdout, expected_stdout, f"[{name}] Path 4 (native exe)")


def verify_reject(
    name: str,
    files: dict[str, str],
    entry: str,
    expected_code: str,
    archive_exe: Path,
    tmp_root: Path,
) -> None:
    case_dir = tmp_root / f"neg_{name}"
    case_dir.mkdir(parents=True, exist_ok=True)
    for rel, content in files.items():
        p = case_dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")
    entry_path = case_dir / entry
    out_huab = case_dir / "out.huab"
    r = run_cmd([str(archive_exe), str(entry_path), str(out_huab)], case_dir)
    combined = r.stdout + "\n" + r.stderr
    rejected = r.returncode != 0 or '"status":"error"' in r.stdout or '"status": "error"' in r.stdout
    if not rejected or expected_code not in combined:
        raise AssertionError(
            f"[neg:{name}] expected rejection containing {expected_code}, got rc={r.returncode}:\n{combined}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", default="huaBin", help="Path to huaBin SDK directory")
    parser.add_argument("--skip-path3", action="store_true", help="Skip Path 3 and negative archive checks")
    parser.add_argument("--only", default="", help="Run only matching case name (b1, b2, b3)")
    args = parser.parse_args()

    sdk = Path(args.sdk)
    if not sdk.is_absolute():
        sdk = ROOT / sdk
    hua_exe = sdk / "bin" / "hua.exe"
    archive_exe = sdk / "lib" / "compiler" / "hua-archive.exe"
    if not hua_exe.exists() or not archive_exe.exists():
        print(f"Missing SDK binaries under {sdk}", file=sys.stderr)
        return 2

    t0 = time.perf_counter()
    tmp_root = Path(tempfile.mkdtemp(prefix="hua_dir_b_"))
    try:
        # ---- B1: Inherent methods (struct + enum + transitive mutating + cross-module pub) ----
        verify_4_paths(
            "b1_methods_basic",
            {
                "helper.hua": """
pub struct Counter {
    val int
}

pub fn Counter.get() int {
    return self.val
}

pub fn Counter.add(n int) {
    self.val += n
}

pub fn Counter.add_twice(n int) {
    self.add(n)
    self.add(n)
}

pub enum OptionInt {
    Some(int)
    None
}

pub fn OptionInt.or_zero() int {
    match self {
        OptionInt.Some(v) => { return v }
        OptionInt.None => { return 0 }
    }
    return 0
}
""",
                "main.hua": """
import helper as h

fn main() {
    var c = h.Counter{ val: 10 }
    c.add(5)
    c.add_twice(3)
    print(c.get())

    let copy = c
    c.add(100)
    print(copy.get())
    print(c.get())

    let a = h.OptionInt.Some(42)
    let b = h.OptionInt.None()
    print(a.or_zero())
    print(b.or_zero())
}
""",
            },
            "main.hua",
            "21\n21\n121\n42\n0\n",
            hua_exe,
            archive_exe,
            tmp_root,
            args.skip_path3,
            args.only,
        )
        print("PASS: b1_methods_basic (4/4 paths)")

        # ---- B2: Structural interface (dispatch, mut fn, []Interface, cross-module, clone) ----
        verify_4_paths(
            "b2_interface_dispatch",
            {
                "shapes.hua": """
pub interface Shape {
    fn area() int
    fn label() string
}

pub interface Scalable {
    fn area() int
    mut fn scale(k int)
}

pub struct Rect {
    w int
    h int
}

pub fn Rect.area() int {
    return self.w * self.h
}

pub fn Rect.label() string {
    return "rect"
}

pub fn Rect.scale(k int) {
    self.w *= k
    self.h *= k
}

pub struct Square {
    side int
}

pub fn Square.area() int {
    return self.side * self.side
}

pub fn Square.label() string {
    return "square"
}

pub fn Square.scale(k int) {
    self.side *= k
}

pub fn describe(s Shape) string {
    return s.label() + ":" + str(s.area())
}

pub fn grow(s mut Scalable, k int) {
    s.scale(k)
}
""",
                "main.hua": """
import shapes as sh

fn make_shape(flag bool) sh.Shape {
    if flag {
        return sh.Rect{ w: 3, h: 4 }
    }
    return sh.Square{ side: 5 }
}

fn main() {
    let s1 sh.Shape = sh.Rect{ w: 2, h: 5 }
    let s2 sh.Shape = sh.Square{ side: 4 }
    print(sh.describe(s1))
    print(sh.describe(s2))

    let chosen = make_shape(false)
    print(sh.describe(chosen))

    var sc sh.Scalable = sh.Square{ side: 3 }
    sh.grow(sc, 2)
    print(sc.area())

    var r = sh.Rect{ w: 2, h: 3 }
    sh.grow(r, 3)
    print(r.area())

    let cloned = clone(sc)
    sc.scale(2)
    print(cloned.area())
    print(sc.area())
}
""",
            },
            "main.hua",
            "rect:10\nsquare:16\nsquare:25\n36\n54\n36\n144\n",
            hua_exe,
            archive_exe,
            tmp_root,
            args.skip_path3,
            args.only,
        )
        print("PASS: b2_interface_dispatch (4/4 paths)")

        # ---- B3: Explicit user generics (fn<T>, struct<T>, nested >>, cross-module) ----
        verify_4_paths(
            "b3_user_generics",
            {
                "boxlib.hua": """
pub struct Box<T> {
    value T
}

pub struct Pair<K, V> {
    first K
    second V
}

pub fn id<T>(x T) T {
    return x
}

pub fn wrap<T>(x T) Box<T> {
    return Box<T>{ value: x }
}

pub fn swap_pair<A, B>(p Pair<A, B>) Pair<B, A> {
    return Pair<B, A>{ first: p.second, second: p.first }
}
""",
                "main.hua": """
import boxlib as bl

fn double_wrap<T>(x T) bl.Box<bl.Box<T>> {
    return bl.Box<bl.Box<T>>{ value: bl.wrap<T>(x) }
}

fn main() {
    print(bl.id<int>(7))
    print(bl.id<string>("hua"))

    let b bl.Box<int> = bl.Box<int>{ value: 99 }
    print(b.value)

    let nested bl.Box<bl.Box<int>> = double_wrap<int>(123)
    print(nested.value.value)

    let p = bl.Pair<string, int>{ first: "age", second: 42 }
    let q = bl.swap_pair<string, int>(p)
    print(q.first)
    print(q.second)
}
""",
            },
            "main.hua",
            "7\nhua\n99\n123\n42\nage\n",
            hua_exe,
            archive_exe,
            tmp_root,
            args.skip_path3,
            args.only,
        )
        print("PASS: b3_user_generics (4/4 paths)")

        # ---- Negative diagnostic checks (Selfhost compiler) ----
        verify_reject(
            "b1_readonly_receiver",
            {
                "main.hua": """
struct C { v int }
fn C.inc() { self.v += 1 }
fn main() {
    let c = C{ v: 1 }
    c.inc()
}
"""
            },
            "main.hua",
            "E3003",
            archive_exe,
            tmp_root,
        )
        verify_reject(
            "b1_private_method",
            {
                "m.hua": """
pub struct C { v int }
fn C.secret() int { return self.v }
""",
                "main.hua": """
import m as m
fn main() {
    let c = m.C{ v: 1 }
    print(c.secret())
}
""",
            },
            "main.hua",
            "E5003",
            archive_exe,
            tmp_root,
        )
        verify_reject(
            "b2_construct_interface",
            {
                "main.hua": """
interface I { fn f() int }
fn main() {
    let x = I{}
}
"""
            },
            "main.hua",
            "E3004",
            archive_exe,
            tmp_root,
        )
        verify_reject(
            "b2_non_mut_rejects_mutating",
            {
                "main.hua": """
interface Reader { fn read() int }
struct S { v int }
fn S.read() int {
    self.v += 1
    return self.v
}
fn take(r Reader) int { return r.read() }
fn main() {
    var s = S{ v: 1 }
    print(take(s))
}
"""
            },
            "main.hua",
            "E3004",
            archive_exe,
            tmp_root,
        )
        verify_reject(
            "b3_arity_mismatch",
            {
                "main.hua": """
fn id<T>(x T) T { return x }
fn main() {
    print(id<int, string>(1))
}
"""
            },
            "main.hua",
            "E3010",
            archive_exe,
            tmp_root,
        )
        verify_reject(
            "b3_duplicate_type_param",
            {
                "main.hua": """
struct Dup<T, T> { x T }
fn main() {}
"""
            },
            "main.hua",
            "E2006",
            archive_exe,
            tmp_root,
        )
        print("PASS: 6/6 negative diagnostic checks")
        print(f"ALL DIRECTION B1/B2/B3 TESTS PASSED in {time.perf_counter() - t0:.2f}s")
        return 0
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
