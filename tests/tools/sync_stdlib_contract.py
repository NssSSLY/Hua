#!/usr/bin/env python3
"""Single-source-of-truth generator and validator for Hua's stdlib contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import audit_stdlib_coverage as audit  # noqa: E402


SCHEMA = "hua.stdlib.manifest.v1"

CORE_BUILTINS_ORDER = [
    "print", "str", "int", "float", "len", "clone", "sqrt", "min", "max", "abs", "clamp",
    "type", "has", "delete", "ok", "err", "is_ok", "is_err", "unwrap", "unwrap_err",
    "unwrap_or", "is_some", "is_none", "byte", "i8", "i16", "i32", "i64", "u8", "u16",
    "u32", "u64", "f32", "f64", "usize", "isize", "sizeof", "alignof",
]

CORE_NATIVE_TYPED = {
    "print", "len", "clone", "has", "delete", "ok", "err", "is_ok", "is_err",
    "unwrap", "unwrap_err", "unwrap_or", "is_some", "is_none",
}

CORE_NATIVE_BRIDGED = [
    "int", "float", "str", "type", "min", "max", "abs", "clamp", "sqrt",
]

STD_NATIVE_INLINE = {
    ("strings", "split"),
    ("strings", "starts_with"),
    ("strings", "ends_with"),
    ("strings", "slice"),
    ("list", "clear"),
    ("list", "extend"),
    ("list", "from_slice"),
    ("list", "len"),
    ("list", "snapshot"),
    ("list", "append"),
    ("list", "get"),
    ("list", "pop"),
    ("list", "set"),
}

MODULE_TYPE_EXPORTS: dict[str, list[str]] = {
    "std.error": ["Value"],
    "std.file": ["File", "IOFailure"],
    "std.stream": ["Reader", "Writer"],
    "std.http": ["Response", "StreamResponse"],
    "std.regex": ["Pattern"],
    "std.process": ["Child"],
    "std.json": ["Value"],
    "std.random": ["Rng"],
    "std.deque": ["Deque"],
}

SELFHOST_TYPE_EXPORTS: dict[str, list[str]] = {
    "std.error": ["Value"],
    "std.file": ["File", "IOFailure"],
    "std.stream": ["Reader", "Writer"],
    "std.http": ["Response", "StreamResponse"],
    "std.regex": ["Pattern"],
    "std.process": ["Child"],
    "std.json": ["Value"],
    "std.random": ["Rng"],
    "std.deque": ["Deque"],
}

STEP2_ENABLE_MODULES = {
    "math",
    "encoding",
    "time",
    "crypto",
    "log",
    "test",
    "gc",
    "process",
    "stream",
    "url",
    "http",
    "regex",
    "compress",
}

STEP5D_NONCALLBACK_MODULES = {
    "random",
    "deque",
    "heap",
    "maps",
    "sets",
    "simd",
    "alg",
    "list",
}

ARCHIVE_BEGIN = "# BEGIN GENERATED: stdlib_load_format"
ARCHIVE_END = "# END GENERATED: stdlib_load_format"
NATIVE_BEGIN = "# BEGIN GENERATED: stdlib_bridged"
NATIVE_END = "# END GENERATED: stdlib_bridged"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def quote_hua(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def hua_str_list(items: list[str]) -> str:
    return "[" + ", ".join(quote_hua(x) for x in items) + "]"


def hua_str_list_compact(items: list[str]) -> str:
    return "[" + ",".join(quote_hua(x) for x in items) + "]"


def polymorphic_rule_for(fn: audit.StdFn) -> str:
    if fn.callback:
        return "callback"
    if fn.module == "math":
        return "math_exact"
    if fn.module == "test" and fn.name in {"equal", "not_equal"}:
        return "test_equal"
    if fn.module == "json" and fn.name in {"encode", "from", "encode_pretty"}:
        return "json_any"
    if fn.module == "alg":
        return "alg"
    if fn.module == "list":
        return "list"
    if fn.module == "task" and fn.name not in {"sleep", "yield", "worker_id"}:
        return "task"
    if "" in fn.params or fn.result == "":
        return f"{fn.module}_poly"
    return "none"


def build_manifest_from_repo(repo: Path, enable_step2: bool = True) -> dict[str, object]:
    host_fns = audit.parse_host_stdlib(repo / "src/stdlib.cpp")
    _, cat_exports, cat_modules, _ = audit.parse_catalog(repo / "selfhost/catalog.hua")
    existing_sigs = audit.parse_signatures(repo / "selfhost/signatures.hua")
    bridged_set, typed_set = audit.parse_native_cpp(repo / "selfhost/native_cpp.hua")

    core_builtins = []
    for name in CORE_BUILTINS_ORDER:
        if name in CORE_NATIVE_TYPED:
            mode = "typed"
        elif name in CORE_NATIVE_BRIDGED:
            mode = "bridged"
        else:
            mode = "pending"
        core_builtins.append({"name": name, "native_mode": mode})

    by_mod: dict[str, list[audit.StdFn]] = {}
    for fn in host_fns:
        by_mod.setdefault(fn.module, []).append(fn)

    modules_out = []
    for mod in sorted(by_mod.keys()):
        mod_full = f"std.{mod}"
        fns_out = []
        for fn in by_mod[mod]:
            sig_key = f"{mod_full}#{fn.name}"
            std_id = f"$std${mod}${fn.name}"
            in_existing_sh = (mod_full in cat_modules) and (fn.name in cat_exports.get(mod_full, set())) and (sig_key in existing_sigs)

            if enable_step2 and (mod in STEP2_ENABLE_MODULES or (mod in STEP5D_NONCALLBACK_MODULES and not fn.callback)):
                sh_enabled = True
                native_mode = "inline" if (mod, fn.name) in STD_NATIVE_INLINE else "bridged"
            else:
                sh_enabled = in_existing_sh
                if (mod, fn.name) in STD_NATIVE_INLINE:
                    native_mode = "inline"
                elif sh_enabled and std_id in bridged_set:
                    native_mode = "bridged"
                else:
                    native_mode = "none"

            if sig_key in existing_sigs:
                sh_params, sh_result, _, _ = existing_sigs[sig_key]
            else:
                sh_params = list(fn.params)
                sh_result = fn.result

            fns_out.append(
                {
                    "name": fn.name,
                    "host_parameters": list(fn.params),
                    "host_result": fn.result,
                    "selfhost_parameters": list(sh_params),
                    "selfhost_result": sh_result,
                    "polymorphic_rule": polymorphic_rule_for(fn),
                    "mutates_first": fn.mutates,
                    "writable_result": fn.writable,
                    "callback": fn.callback,
                    "since": fn.since,
                    "backends": {
                        "host": True,
                        "selfhost_check": sh_enabled,
                        "selfhost_huab": sh_enabled,
                        "native_release": native_mode,
                    },
                }
            )

        modules_out.append(
            {
                "module": mod_full,
                "short_name": mod,
                "exported_types": list(MODULE_TYPE_EXPORTS.get(mod_full, [])),
                "selfhost_exported_types": list(SELFHOST_TYPE_EXPORTS.get(mod_full, [])),
                "functions": fns_out,
            }
        )

    return {
        "schema": SCHEMA,
        "core_builtins": core_builtins,
        "source_modules": [
            {
                "module": "std.alg_integer",
                "repo_path": "std/alg_integer.hua",
                "sdk_path": "src/std/alg_integer.hua",
                "since": 6,
            },
            {
                "module": "std.sse",
                "repo_path": "std/sse.hua",
                "sdk_path": "src/std/sse.hua",
                "since": 31,
            },
            {
                "module": "std.csv",
                "repo_path": "std/csv.hua",
                "sdk_path": "src/std/csv.hua",
                "since": 32,
            },
        ],
        "modules": modules_out,
    }


def render_catalog(manifest: dict[str, object]) -> str:
    core_names = [item["name"] for item in manifest["core_builtins"]]
    sh_modules = []
    export_lines = []
    type_clauses = []

    for mod_entry in manifest["modules"]:
        mod_full = mod_entry["module"]
        sh_fns = [f["name"] for f in mod_entry["functions"] if f["backends"]["selfhost_check"]]
        sh_types = list(mod_entry.get("selfhost_exported_types", []))
        if not sh_fns and not sh_types:
            continue
        sh_modules.append(mod_full)
        # Preserve type exports in module exports list
        all_exports = list(sh_types) + sh_fns
        export_lines.append(f"    if module == {quote_hua(mod_full)} {{ return {hua_str_list(all_exports)} }}")
        if len(sh_types) == 1:
            type_clauses.append(f"(module == {quote_hua(mod_full)} && name == {quote_hua(sh_types[0])})")
        elif len(sh_types) > 1:
            inner = " || ".join(f"name == {quote_hua(t)}" for t in sh_types)
            type_clauses.append(f"(module == {quote_hua(mod_full)} && ({inner}))")

    type_expr = " || ".join(type_clauses) if type_clauses else "false"
    lines = [
        "# GENERATED from selfhost/stdlib_manifest.json by tests/tools/sync_stdlib_contract.py --write. Do not edit by hand.",
        "import std.alg as alg",
        f"pub fn core(name string) bool {{ return alg.contains({hua_str_list(core_names)},name) }}",
        'pub fn primitive(name string) bool { return alg.contains(["int","float","bool","string","byte","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","usize","isize","void","Json","Bytes","Buffer","List","Result","Task","NativeHandle","map"],name) }',
        f"pub fn module(name string) bool {{ return alg.contains({hua_str_list(sh_modules)},name) }}",
        "pub fn exports(module string) []string {",
        *export_lines,
        "    var empty []string = []",
        "    return empty",
        "}",
        f"pub fn type_export(module string, name string) bool {{ return {type_expr} }}",
    ]
    return "\n".join(lines) + "\n"


def render_signatures(manifest: dict[str, object]) -> str:
    lines = [
        "# GENERATED from selfhost/stdlib_manifest.json by tests/tools/sync_stdlib_contract.py --write. Do not edit by hand.",
        "import std.strings as strings",
        "pub struct Signature { parameters []string",
        "    result string",
        "    mutates bool",
        "    writable bool }",
        "pub fn signature(target string) Signature? {",
        '    let parts = strings.split(target, "#")',
        "    if len(parts) != 2 { return nil }",
        "    let module = parts[0]",
        "    let name = parts[1]",
    ]
    for mod_entry in manifest["modules"]:
        mod_full = mod_entry["module"]
        sh_fns = [f for f in mod_entry["functions"] if f["backends"]["selfhost_check"]]
        if not sh_fns:
            continue
        lines.append(f'    if module == "builtin:{mod_full}" {{')
        for f in sh_fns:
            params = hua_str_list_compact(f["selfhost_parameters"])
            res = quote_hua(f["selfhost_result"])
            mut_s = "true" if f["mutates_first"] else "false"
            writ_s = "true" if f["writable_result"] else "false"
            lines.append(
                f'        if name == {quote_hua(f["name"])} '
                f"{{ return Signature{{parameters:{params},result:{res},mutates:{mut_s},writable:{writ_s}}} }}"
            )
        lines.append("        return nil")
        lines.append("    }")
    lines.append("    return nil")
    lines.append("}")
    return "\n".join(lines) + "\n"


def render_archive_load_format(manifest: dict[str, object]) -> str:
    lines = [
        ARCHIVE_BEGIN,
        "fn load_format(text string) int {",
    ]
    for mod_entry in manifest["modules"]:
        short = mod_entry["short_name"]
        huab_fns = [f for f in mod_entry["functions"] if f["backends"]["selfhost_huab"]]
        if not huab_fns:
            continue
        effective = [(f["name"], max(6, int(f["since"]))) for f in huab_fns]
        max_ver = max(v for _, v in effective)
        if max_ver <= 6:
            continue
        min_ver = min(v for _, v in effective)
        prefix = f"$core$$std${short}$"
        if min_ver == max_ver:
            lines.append(f"    if strings.starts_with(text,{quote_hua(prefix)}) {{ return {max_ver} }}")
        else:
            buckets: dict[int, list[str]] = {}
            for fname, ver in effective:
                if ver > min_ver:
                    buckets.setdefault(ver, []).append(fname)
            lines.append(f"    if strings.starts_with(text,{quote_hua(prefix)}) {{")
            lines.append(f"        let name=rules.cut(text,{len(prefix)},len(text))")
            for ver in sorted(buckets.keys(), reverse=True):
                lines.append(f"        if alg.contains({hua_str_list_compact(buckets[ver])},name) {{ return {ver} }}")
            lines.append(f"        return {min_ver}")
            lines.append("    }")
    lines.append("    return 6")
    lines.append("}")
    lines.append(ARCHIVE_END)
    return "\n".join(lines)


def render_native_bridged(manifest: dict[str, object]) -> str:
    core_bridged = [item["name"] for item in manifest["core_builtins"] if item["native_mode"] == "bridged"]
    lines = [
        NATIVE_BEGIN,
        "fn bridged(name string) bool {",
        '    if !strings.starts_with(name,"$std$") {',
        f"        return alg.contains({hua_str_list_compact(core_bridged)},name)",
        "    }",
        '    let parts=strings.split(name,"$")',
        "    if len(parts)!=4 { return false }",
        "    let module=parts[2]",
        "    let func=parts[3]",
    ]
    for mod_entry in manifest["modules"]:
        short = mod_entry["short_name"]
        b_fns = [f["name"] for f in mod_entry["functions"] if f["backends"]["native_release"] == "bridged"]
        if not b_fns:
            continue
        lines.append(f"    if module=={quote_hua(short)} {{ return alg.contains({hua_str_list_compact(b_fns)},func) }}")
    lines.append("    return false")
    lines.append("}")
    lines.append(NATIVE_END)
    return "\n".join(lines)


def replace_generated_block(text: str, begin: str, end: str, replacement: str, fallback_regex: str) -> str:
    if begin in text and end in text:
        pattern = re.compile(re.escape(begin) + r".*?" + re.escape(end), re.S)
        return pattern.sub(lambda _: replacement, text, count=1)
    m = re.search(fallback_regex, text, re.S)
    if not m:
        raise ValueError(f"Could not find target block for {begin}")
    return text[: m.start()] + replacement + text[m.end() :]


def render_archive_file(existing_text: str, manifest: dict[str, object]) -> str:
    block = render_archive_load_format(manifest)
    return replace_generated_block(
        existing_text,
        ARCHIVE_BEGIN,
        ARCHIVE_END,
        block,
        r"fn load_format\(text string\) int \{.*?\n\}",
    )


def render_native_cpp_file(existing_text: str, manifest: dict[str, object]) -> str:
    block = render_native_bridged(manifest)
    return replace_generated_block(
        existing_text,
        NATIVE_BEGIN,
        NATIVE_END,
        block,
        r"fn bridged\(name string\) bool \{.*?\n\}",
    )


def validate_manifest_and_repo(repo: Path, check_sdk: bool) -> list[str]:
    errors: list[str] = []
    manifest_path = repo / "selfhost/stdlib_manifest.json"
    if not manifest_path.is_file():
        return [f"Missing manifest: {manifest_path}"]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    if manifest.get("schema") != SCHEMA:
        errors.append(f"Invalid manifest schema: {manifest.get('schema')!r} (expected {SCHEMA!r})")

    # 1. Compare against src/stdlib.cpp
    host_fns = audit.parse_host_stdlib(repo / "src/stdlib.cpp")
    host_map: dict[tuple[str, str], audit.StdFn] = {(f"std.{f.module}", f.name): f for f in host_fns}
    manifest_map: dict[tuple[str, str], dict[str, object]] = {}

    for mod_entry in manifest.get("modules", []):
        mod_full = mod_entry["module"]
        for f in mod_entry.get("functions", []):
            key = (mod_full, f["name"])
            if key in manifest_map:
                errors.append(f"Duplicate manifest entry: {mod_full}.{f['name']}")
            manifest_map[key] = f

    for key, hfn in sorted(host_map.items()):
        full = f"{key[0]}.{key[1]}"
        if key not in manifest_map:
            errors.append(f"Missing host function in stdlib_manifest.json: {full}")
            continue
        mf = manifest_map[key]
        if mf.get("host_parameters") != hfn.params:
            errors.append(f"{full}: host_parameters mismatch (manifest={mf.get('host_parameters')}, host={hfn.params})")
        if mf.get("host_result") != hfn.result:
            errors.append(f"{full}: host_result mismatch (manifest={mf.get('host_result')!r}, host={hfn.result!r})")
        if bool(mf.get("mutates_first")) != hfn.mutates:
            errors.append(f"{full}: mutates_first mismatch (manifest={mf.get('mutates_first')}, host={hfn.mutates})")
        if bool(mf.get("writable_result")) != hfn.writable:
            errors.append(f"{full}: writable_result mismatch (manifest={mf.get('writable_result')}, host={hfn.writable})")
        if bool(mf.get("callback")) != hfn.callback:
            errors.append(f"{full}: callback mismatch (manifest={mf.get('callback')}, host={hfn.callback})")
        if int(mf.get("since", -1)) != hfn.since:
            errors.append(f"{full}: since mismatch (manifest={mf.get('since')}, host={hfn.since})")

        # Internal backend consistency
        be = mf.get("backends", {})
        if be.get("host") is not True:
            errors.append(f"{full}: backends.host must be true")
        if bool(be.get("selfhost_huab")) != bool(be.get("selfhost_check")):
            errors.append(f"{full}: backends.selfhost_huab must equal selfhost_check")
        nat = be.get("native_release")
        if nat not in {"none", "bridged", "inline"}:
            errors.append(f"{full}: invalid backends.native_release {nat!r}")
        if nat != "none":
            if not be.get("selfhost_check"):
                errors.append(f"{full}: native_release={nat!r} requires selfhost_check=true")
            if mf.get("polymorphic_rule") == "none":
                for p in mf.get("selfhost_parameters", []):
                    if not audit.native_cpp_supports_type(p):
                        errors.append(f"{full}: unsupported native parameter type {p!r}")
                if not audit.native_cpp_supports_type(str(mf.get("selfhost_result", ""))):
                    errors.append(f"{full}: unsupported native result type {mf.get('selfhost_result')!r}")

    for key in sorted(manifest_map.keys()):
        if key not in host_map:
            errors.append(f"Extra function in stdlib_manifest.json not in src/stdlib.cpp: {key[0]}.{key[1]}")

    # 2. Check source_modules exist
    for sm in manifest.get("source_modules", []):
        repo_path = repo / sm["repo_path"]
        if not repo_path.is_file():
            errors.append(f"Source module {sm['module']} missing at {repo_path}")

    # 3. Byte-for-byte check of 4 generated selfhost files
    expected_catalog = render_catalog(manifest)
    actual_catalog = (repo / "selfhost/catalog.hua").read_text(encoding="utf-8")
    if actual_catalog != expected_catalog:
        errors.append("Drift detected in selfhost/catalog.hua (run sync_stdlib_contract.py --write)")

    expected_sigs = render_signatures(manifest)
    actual_sigs = (repo / "selfhost/signatures.hua").read_text(encoding="utf-8")
    if actual_sigs != expected_sigs:
        errors.append("Drift detected in selfhost/signatures.hua (run sync_stdlib_contract.py --write)")

    actual_archive = (repo / "selfhost/archive.hua").read_text(encoding="utf-8")
    if ARCHIVE_BEGIN not in actual_archive or ARCHIVE_END not in actual_archive:
        errors.append("Missing generated markers in selfhost/archive.hua")
    else:
        expected_archive = render_archive_file(actual_archive, manifest)
        if actual_archive != expected_archive:
            errors.append("Drift detected in selfhost/archive.hua load_format (run sync_stdlib_contract.py --write)")

    actual_native = (repo / "selfhost/native_cpp.hua").read_text(encoding="utf-8")
    if NATIVE_BEGIN not in actual_native or NATIVE_END not in actual_native:
        errors.append("Missing generated markers in selfhost/native_cpp.hua")
    else:
        expected_native = render_native_cpp_file(actual_native, manifest)
        if actual_native != expected_native:
            errors.append("Drift detected in selfhost/native_cpp.hua bridged() (run sync_stdlib_contract.py --write)")

    # 4. Optional huaBin SDK sync check
    if check_sdk:
        sdk = repo / "huaBin"
        sdk_manifest_path = sdk / "manifest.json"
        if sdk_manifest_path.is_file():
            for rel in (
                "stdlib_manifest.json",
                "catalog.hua",
                "signatures.hua",
                "archive.hua",
                "native_cpp.hua",
                "types.hua",
            ):
                src_file = repo / "selfhost" / rel
                dst_file = sdk / "src/compiler" / rel
                if not dst_file.is_file():
                    errors.append(f"SDK missing {dst_file.relative_to(repo).as_posix()}")
                elif sha256_bytes(src_file.read_bytes()) != sha256_bytes(dst_file.read_bytes()):
                    errors.append(f"SDK drift: {dst_file.relative_to(repo).as_posix()} != selfhost/{rel}")
            sdk_manifest = json.loads(sdk_manifest_path.read_text(encoding="utf-8"))
            sdk_source_mods = sdk_manifest.get("source_modules", {})
            for sm in manifest.get("source_modules", []):
                if sdk_source_mods.get(sm["module"]) != sm["sdk_path"]:
                    errors.append(
                        f"SDK manifest source_modules mismatch for {sm['module']}: "
                        f"{sdk_source_mods.get(sm['module'])!r} != {sm['sdk_path']!r}"
                    )

    return errors


def run_negative_self_tests(repo: Path) -> None:
    """Verify --check catches missing backends, tampered signatures, tampered since, and stale generated files."""
    with tempfile.TemporaryDirectory(prefix="hua-stdlib-contract-") as tmp:
        stage = Path(tmp)
        for rel in (
            "src/stdlib.cpp",
            "std/alg_integer.hua",
            "std/sse.hua",
            "std/csv.hua",
            "selfhost/stdlib_manifest.json",
            "selfhost/catalog.hua",
            "selfhost/signatures.hua",
            "selfhost/archive.hua",
            "selfhost/native_cpp.hua",
        ):
            dst = stage / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(repo / rel, dst)

        baseline = validate_manifest_and_repo(stage, check_sdk=False)
        if baseline:
            raise AssertionError(f"Self-test baseline failed: {baseline}")

        # Case 1: Tamper signature in manifest (std.math.gcd)
        m_path = stage / "selfhost/stdlib_manifest.json"
        orig_manifest_bytes = m_path.read_bytes()
        data = json.loads(orig_manifest_bytes.decode("utf-8"))
        for mod in data["modules"]:
            if mod["short_name"] == "math":
                for f in mod["functions"]:
                    if f["name"] == "gcd":
                        f["host_parameters"] = ["float", "float"]
        m_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        errs = validate_manifest_and_repo(stage, check_sdk=False)
        assert any("std.math.gcd" in e and "host_parameters" in e for e in errs), f"Expected gcd error, got {errs}"
        m_path.write_bytes(orig_manifest_bytes)

        # Case 2: Tamper since in manifest (std.process.run)
        data = json.loads(orig_manifest_bytes.decode("utf-8"))
        for mod in data["modules"]:
            if mod["short_name"] == "process":
                for f in mod["functions"]:
                    if f["name"] == "run":
                        f["since"] = 6
        m_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        errs = validate_manifest_and_repo(stage, check_sdk=False)
        assert any("std.process.run" in e and "since" in e for e in errs), f"Expected process.run since error, got {errs}"
        m_path.write_bytes(orig_manifest_bytes)

        # Case 3: Drop a bridged function from native_cpp.hua
        n_path = stage / "selfhost/native_cpp.hua"
        orig_native = n_path.read_text(encoding="utf-8")
        n_path.write_text(orig_native.replace('"sha256",', ""), encoding="utf-8")
        errs = validate_manifest_and_repo(stage, check_sdk=False)
        assert any("selfhost/native_cpp.hua" in e for e in errs), f"Expected native_cpp drift, got {errs}"
        n_path.write_text(orig_native, encoding="utf-8")

        # Case 4: Drop a version gate from archive.hua
        a_path = stage / "selfhost/archive.hua"
        orig_archive = a_path.read_text(encoding="utf-8")
        a_path.write_text(orig_archive.replace("return 28", "return 6", 1), encoding="utf-8")
        errs = validate_manifest_and_repo(stage, check_sdk=False)
        assert any("selfhost/archive.hua" in e for e in errs), f"Expected archive drift, got {errs}"
        a_path.write_text(orig_archive, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".", help="Repository root")
    parser.add_argument("--bootstrap-manifest", action="store_true", help="Initialize selfhost/stdlib_manifest.json from repo")
    parser.add_argument("--write", action="store_true", help="Regenerate selfhost tables from selfhost/stdlib_manifest.json")
    parser.add_argument("--check", action="store_true", help="Validate manifest and generated tables")
    parser.add_argument("--self-test", action="store_true", help="Run negative drift tests in a temp directory")
    parser.add_argument("--skip-sdk", action="store_true", help="Skip huaBin SDK synchronization check")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    manifest_path = repo / "selfhost/stdlib_manifest.json"

    if args.bootstrap_manifest:
        manifest = build_manifest_from_repo(repo, enable_step2=True)
        manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"Wrote {manifest_path}")

    if args.write:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        (repo / "selfhost/catalog.hua").write_text(render_catalog(manifest), encoding="utf-8")
        (repo / "selfhost/signatures.hua").write_text(render_signatures(manifest), encoding="utf-8")
        archive_path = repo / "selfhost/archive.hua"
        archive_path.write_text(render_archive_file(archive_path.read_text(encoding="utf-8"), manifest), encoding="utf-8")
        native_path = repo / "selfhost/native_cpp.hua"
        native_path.write_text(render_native_cpp_file(native_path.read_text(encoding="utf-8"), manifest), encoding="utf-8")
        print("Regenerated selfhost/catalog.hua, signatures.hua, archive.hua, native_cpp.hua")

    if args.check:
        errs = validate_manifest_and_repo(repo, check_sdk=not args.skip_sdk)
        if errs:
            for e in errs:
                print(f"ERROR: {e}", file=sys.stderr)
            sys.exit(1)
        print("stdlib_contract check passed")

    if args.self_test:
        run_negative_self_tests(repo)
        print("stdlib_contract negative self-tests passed (4/4)")


if __name__ == "__main__":
    main()
