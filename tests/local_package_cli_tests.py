#!/usr/bin/env python3
"""Step 1 acceptance: out-of-repo multi-package Hua CLI across hua run, HUAB, and hua build --release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


def write_utf8(path: Path, content: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")
    return path


def clean_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    sys_root = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = str(Path(sys_root) / "System32")
    env.pop("HUA_HOME", None)
    env.pop("HUA_PATH", None)
    if extra:
        env.update(extra)
    return env


def run_cmd(argv: list[str | Path], cwd: Path, env: dict[str, str], timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in argv],
        cwd=str(cwd),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
    )


def create_multi_package_workspace(base: Path) -> Path:
    # Package 1: logschema (record validation + accumulation + structured errors)
    pkg1 = base / "packages" / "logschema"
    write_utf8(
        pkg1 / "hua.package.json",
        json.dumps({"schema": "hua.package.v1", "name": "logschema", "version": "0.1.0"}, indent=2) + "\n",
    )
    write_utf8(
        pkg1 / "rules.hua",
        """import std.error as error
import std.json as json
import std.strings as strings

pub struct Policy {
    min_level int
    allow_empty_service bool
}

pub struct Event {
    service string
    level int
    latency_ms int
}

pub fn check_record(item Json, policy Policy, line_no int) Result<Event, error.Value> {
    let raw_svc = json.get(item, "service")
    if is_err(raw_svc) {
        let base = unwrap(error.make("logschema.record", "missing_field", "missing field service"))
        return err(error.with_context(base, unwrap(strings.format("jsonl line {}", [str(line_no)]))))
    }
    let svc_str = json.as_string(unwrap(raw_svc))
    if is_err(svc_str) {
        let base = unwrap(error.make("logschema.record", "bad_type", "service must be string"))
        return err(error.with_context(base, unwrap(strings.format("jsonl line {}", [str(line_no)]))))
    }
    let svc = strings.lower_ascii(strings.trim(unwrap(svc_str)))
    if strings.is_empty(svc) && !policy.allow_empty_service {
        let base = unwrap(error.make("logschema.record", "empty_service", "service cannot be empty"))
        return err(error.with_context(base, unwrap(strings.format("jsonl line {}", [str(line_no)]))))
    }
    let raw_lvl = json.get(item, "level")
    if is_err(raw_lvl) {
        let base = unwrap(error.make("logschema.record", "missing_field", "missing field level"))
        return err(error.with_context(base, unwrap(strings.format("jsonl line {}", [str(line_no)]))))
    }
    let lvl = json.as_int(unwrap(raw_lvl))
    if is_err(lvl) {
        let base = unwrap(error.make("logschema.record", "bad_type", "level must be int"))
        return err(error.with_context(base, unwrap(strings.format("jsonl line {}", [str(line_no)]))))
    }
    let raw_lat = json.get(item, "latency_ms")
    if is_err(raw_lat) {
        let base = unwrap(error.make("logschema.record", "missing_field", "missing field latency_ms"))
        return err(error.with_context(base, unwrap(strings.format("jsonl line {}", [str(line_no)]))))
    }
    let lat = json.as_int(unwrap(raw_lat))
    if is_err(lat) || unwrap(lat) < 0 {
        let base = unwrap(error.make("logschema.record", "bad_latency", "latency_ms must be non-negative int"))
        return err(error.with_context(base, unwrap(strings.format("jsonl line {}", [str(line_no)]))))
    }
    return ok(Event{service: svc, level: unwrap(lvl), latency_ms: unwrap(lat)})
}
""",
    )
    write_utf8(
        pkg1 / "package.hua",
        """import rules as rules
import std.error as error
import std.json as json
import std.list as list

var load_counter = 0

fn bump() int {
    load_counter += 1
    return load_counter
}

let initialized_once = bump()

pub struct Summary {
    total_lines int
    kept_events int
    total_latency int
    by_service map[string]int
    init_count int
}

pub fn make_policy(min_level int, allow_empty bool) rules.Policy {
    return rules.Policy{min_level: min_level, allow_empty_service: allow_empty}
}

pub fn summarize(lines []string, policy rules.Policy) Result<Summary, error.Value> {
    var by_svc = map[string]int{}
    var kept = 0
    var sum_lat = 0
    for i in 0..len(lines) {
        let decoded = json.decode(lines[i])
        if is_err(decoded) {
            let base = unwrap(error.make("logschema.syntax", "invalid_json", unwrap_err(decoded)))
            return err(error.with_context(base, "jsonl line " + str(i + 1)))
        }
        let ev = rules.check_record(unwrap(decoded), policy, i + 1)?
        if ev.level >= policy.min_level {
            kept += 1
            sum_lat += ev.latency_ms
            by_svc[ev.service] = unwrap_or(by_svc[ev.service], 0) + 1
        }
    }
    return ok(Summary{
        total_lines: len(lines),
        kept_events: kept,
        total_latency: sum_lat,
        by_service: by_svc,
        init_count: load_counter
    })
}
""",
    )

    # Package 2: reportfmt (depends on logschema -> diamond dependency with root CLI!)
    pkg2 = base / "packages" / "reportfmt"
    write_utf8(
        pkg2 / "hua.package.json",
        json.dumps(
            {
                "schema": "hua.package.v1",
                "name": "reportfmt",
                "version": "0.1.0",
                "deps": {"logschema": "../logschema"},
            },
            indent=2,
        )
        + "\n",
    )
    write_utf8(
        pkg2 / "package.hua",
        """import logschema as schema
import std.error as error
import std.json as json
import std.strings as strings

pub fn render_json(title string, s schema.Summary) Result<string, error.Value> {
    var svc_json = map[string]Json{}
    for k, v in s.by_service {
        svc_json[k] = unwrap(json.from(v))
    }
    var root = json.object(map[string]Json{
        "title": unwrap(json.from(title)),
        "total_lines": unwrap(json.from(s.total_lines)),
        "kept_events": unwrap(json.from(s.kept_events)),
        "total_latency": unwrap(json.from(s.total_latency)),
        "schema_inits": unwrap(json.from(s.init_count)),
        "services": json.object(svc_json)
    })
    let formatted = json.encode_pretty(root, 2)
    if is_err(formatted) {
        return err(unwrap(error.make("reportfmt", "encode", unwrap_err(formatted))))
    }
    return ok(unwrap(formatted) + "\\n")
}

pub fn banner(title string, s schema.Summary) string {
    return unwrap(strings.template("{title}: kept={kept}/{total} latency={lat}ms (inits={inits})", map[string]string{
        "title": title,
        "kept": str(s.kept_events),
        "total": str(s.total_lines),
        "lat": strings.format_int_grouped(s.total_latency, ","),
        "inits": str(s.init_count)
    }))
}
""",
    )

    # Root CLI App: audit_cli (depends on both logschema and reportfmt)
    app = base / "工具 项目" / "audit_cli"
    write_utf8(
        app / "hua.package.json",
        json.dumps(
            {
                "schema": "hua.package.v1",
                "name": "audit_cli",
                "version": "0.1.0",
                "deps": {
                    "logschema": "../../packages/logschema",
                    "reportfmt": "../../packages/reportfmt",
                },
            },
            indent=2,
        )
        + "\n",
    )
    write_utf8(
        app / "config.hua",
        """import logschema as schema
import logschema.rules
import std.error as error
import std.fs as fs
import std.json as json
import std.strings as strings

pub struct AppConfig {
    title string
    policy logschema.rules.Policy
}

pub fn load(filename string, strict_override bool) Result<AppConfig, error.Value> {
    let raw = fs.read_text(filename)
    if is_err(raw) {
        return err(unwrap(error.make("audit.config", "io", unwrap_err(raw))))
    }
    let parsed = json.decode(unwrap(raw))
    if is_err(parsed) {
        return err(unwrap(error.make("audit.config", "json", unwrap_err(parsed))))
    }
    let doc = unwrap(parsed)
    let title_node = json.get(doc, "title")
    if is_err(title_node) {
        return err(unwrap(error.make("audit.config", "missing_title", "config requires title")))
    }
    let title = json.as_string(unwrap(title_node))
    if is_err(title) {
        return err(unwrap(error.make("audit.config", "bad_title", "title must be string")))
    }
    let lvl_node = json.get(doc, "min_level")
    if is_err(lvl_node) {
        return err(unwrap(error.make("audit.config", "missing_level", "config requires min_level")))
    }
    let min_lvl = json.as_int(unwrap(lvl_node))
    if is_err(min_lvl) {
        return err(unwrap(error.make("audit.config", "bad_level", "min_level must be int")))
    }
    var allow_empty = !strict_override
    let allow_node = json.get(doc, "allow_empty_service")
    if is_ok(allow_node) && !strict_override {
        allow_empty = unwrap_or(json.as_bool(unwrap(allow_node)), false)
    }
    return ok(AppConfig{
        title: strings.trim(unwrap(title)),
        policy: schema.make_policy(unwrap(min_lvl), allow_empty)
    })
}
""",
    )
    write_utf8(
        app / "main.hua",
        """import config as config
import logschema as schema
import logschema.rules as rules
import reportfmt as reportfmt
import std.error as error
import std.file as file
import std.fs as fs
import std.list as list
import std.os as os
import std.path as path
import std.strings as strings

fn close_quietly(handle file.File) {
    file.close(handle)
}

fn read_nonempty_lines(filename string) Result<[]string, error.Value> {
    let opened = file.open(filename, "r")
    if is_err(opened) {
        return err(error.with_context(unwrap_err(opened), "open input jsonl"))
    }
    let handle = unwrap(opened)
    defer close_quietly(handle)
    let empty []string = []
    var lines = list.from_slice(empty)
    while true {
        let next = file.read_line(handle)
        if is_err(next) {
            return err(file.error(unwrap_err(next)))
        }
        let line_opt = unwrap(next)
        if is_none(line_opt) { break }
        let trimmed = strings.trim(unwrap(line_opt))
        if !strings.is_empty(trimmed) {
            let added = list.append(lines, trimmed)
            if is_err(added) {
                return err(unwrap(error.make("audit.input", "limit", unwrap_err(added))))
            }
        }
    }
    return ok(list.snapshot(lines))
}

fn write_atomic(target string, content string) Result<void, error.Value> {
    var dir = unwrap_or(path.dirname(target), ".")
    if dir == "" { dir = "." }
    let mk = fs.mkdir_all(dir)
    if is_err(mk) { return err(unwrap(error.make("audit.output", "mkdir", unwrap_err(mk)))) }
    let tmp = fs.temp_file(dir, ".audit-out-")
    if is_err(tmp) { return err(unwrap(error.make("audit.output", "temp", unwrap_err(tmp)))) }
    let temp_path = unwrap(tmp)
    let wrote = fs.write_text(temp_path, content)
    if is_err(wrote) {
        fs.remove(temp_path)
        return err(unwrap(error.make("audit.output", "write", unwrap_err(wrote))))
    }
    let rep = fs.replace(temp_path, target)
    if is_err(rep) {
        fs.remove(temp_path)
        return err(unwrap(error.make("audit.output", "replace", unwrap_err(rep))))
    }
    return ok()
}

fn main() Result<void, error.Value> {
    let parsed = os.parse_args(os.args(), ["strict"], ["config", "input", "output"])
    if is_err(parsed) {
        return err(unwrap(error.make("hua.cli", "usage", unwrap_err(parsed))))
    }
    let opts = unwrap(parsed)
    let cfg_vals = unwrap_or(opts["config"], [])
    let in_vals = unwrap_or(opts["input"], [])
    let out_vals = unwrap_or(opts["output"], [])
    if len(cfg_vals) != 1 || len(in_vals) != 1 {
        return err(unwrap(error.make("hua.cli", "usage", "expected --config <file> and --input <file>")))
    }
    let strict = len(unwrap_or(opts["strict"], [])) > 0
    let cfg = config.load(cfg_vals[0], strict)?
    let lines = read_nonempty_lines(in_vals[0])?
    let summary = schema.summarize(lines, cfg.policy)?
    let report = reportfmt.render_json(cfg.title, summary)?
    if len(out_vals) == 1 {
        write_atomic(out_vals[0], report)?
    }
    # Directly reference rules.Policy via imported sub-module to verify sub-module type identity.
    let check_pol rules.Policy = cfg.policy
    if check_pol.min_level < -1000 { print("unreachable") }
    print(reportfmt.banner(cfg.title, summary))
    return ok()
}
""",
    )
    return app / "main.hua"


def extract_error_locations(stderr: str) -> list[str]:
    primary = re.findall(r"--> ([^\r\n]+)", stderr)
    notes = re.findall(r"^note: .*? at ([^\r\n]+)$", stderr, flags=re.MULTILINE)
    return primary + notes


def verify_diagnostics_parity(sdk: Path, work: Path, env: dict[str, str]) -> int:
    host = sdk / "bin/hua.exe"
    archive_exe = sdk / "lib/compiler/hua-archive.exe"
    diag_root = work / "diagnostics"
    diag_root.mkdir(parents=True, exist_ok=True)
    checks = 0

    def check_case(name: str, files: dict[str, str], entry_rel: str, expected_code: str) -> None:
        nonlocal checks
        case_dir = diag_root / name
        for rel, txt in files.items():
            write_utf8(case_dir / rel, txt)
        entry = case_dir / entry_rel
        # 1. Host check & run
        r_check = run_cmd([host, "check", entry], case_dir, env)
        r_run = run_cmd([host, "run", entry], case_dir, env)
        # 2. Release build
        r_rel = run_cmd([host, "build", "--release", entry, "-o", case_dir / "out.exe"], case_dir, env)
        # 3. Selfhost native archive compiler
        r_arc = run_cmd([archive_exe, entry, case_dir / "out.huab"], case_dir, env | {"HUA_HOME": str(sdk)})
        arc_json = json.loads(r_arc.stdout)

        assert r_check.returncode == 1 and f"error[{expected_code}]" in r_check.stderr, (name, "check", r_check)
        assert r_run.returncode == 1 and f"error[{expected_code}]" in r_run.stderr, (name, "run", r_run)
        assert r_rel.returncode == 1 and f"error[{expected_code}]" in r_rel.stderr, (name, "release", r_rel)
        assert arc_json["status"] == "error" and expected_code in arc_json["error"], (name, "archive", arc_json)
        assert not (case_dir / "out.exe").exists() and not (case_dir / "out.huab").exists(), name
        checks += 4

    # Case 1: Missing dependency directory -> E5001
    check_case(
        "missing_dep_dir",
        {
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"app","version":"0.1.0","deps":{"missing":"../nope"}}\n',
            "app/main.hua": "import missing\nfn main() {}\n",
        },
        "app/main.hua",
        "E5001",
    )

    # Case 2: Package missing package.hua -> E5001
    check_case(
        "missing_package_hua",
        {
            "pkg/hua.package.json": '{"schema":"hua.package.v1","name":"pkg","version":"0.1.0"}\n',
            "pkg/other.hua": "pub fn f() int { return 1 }\n",
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"app","version":"0.1.0","deps":{"pkg":"../pkg"}}\n',
            "app/main.hua": "import pkg\nfn main() {}\n",
        },
        "app/main.hua",
        "E5001",
    )

    # Case 3: Reserved 'std' package name -> E5004
    check_case(
        "reserved_std_name",
        {
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"std","version":"0.1.0"}\n',
            "app/main.hua": "fn main() {}\n",
        },
        "app/main.hua",
        "E5004",
    )

    # Case 4: Manifest name mismatch -> E5004
    check_case(
        "manifest_name_mismatch",
        {
            "pkg/hua.package.json": '{"schema":"hua.package.v1","name":"actual_name","version":"0.1.0"}\n',
            "pkg/package.hua": "pub fn f() int { return 1 }\n",
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"app","version":"0.1.0","deps":{"wrong_alias":"../pkg"}}\n',
            "app/main.hua": "import wrong_alias\nfn main() {}\n",
        },
        "app/main.hua",
        "E5004",
    )

    # Case 5: Local file conflicts with package dependency -> E5004
    check_case(
        "local_dep_conflict",
        {
            "pkg/hua.package.json": '{"schema":"hua.package.v1","name":"pkg","version":"0.1.0"}\n',
            "pkg/package.hua": "pub fn f() int { return 1 }\n",
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"app","version":"0.1.0","deps":{"pkg":"../pkg"}}\n',
            "app/pkg.hua": "pub fn f() int { return 2 }\n",
            "app/main.hua": "import pkg\nfn main() { print(pkg.f()) }\n",
        },
        "app/main.hua",
        "E5004",
    )

    # Case 6: Transitive diamond conflict (same name, different canonical directories) -> E5004
    check_case(
        "diamond_conflict",
        {
            "u1/hua.package.json": '{"schema":"hua.package.v1","name":"util","version":"0.1.0"}\n',
            "u1/package.hua": "pub fn v() int { return 1 }\n",
            "u2/hua.package.json": '{"schema":"hua.package.v1","name":"util","version":"0.2.0"}\n',
            "u2/package.hua": "pub fn v() int { return 2 }\n",
            "b/hua.package.json": '{"schema":"hua.package.v1","name":"b","version":"0.1.0","deps":{"util":"../u1"}}\n',
            "b/package.hua": "import util\npub fn x() int { return util.v() }\n",
            "c/hua.package.json": '{"schema":"hua.package.v1","name":"c","version":"0.1.0","deps":{"util":"../u2"}}\n',
            "c/package.hua": "import util\npub fn y() int { return util.v() }\n",
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"app","version":"0.1.0","deps":{"b":"../b","c":"../c"}}\n',
            "app/main.hua": "import b\nimport c\nfn main() { print(b.x(), c.y()) }\n",
        },
        "app/main.hua",
        "E5004",
    )

    # Case 7: Duplicate key in hua.package.json -> E5004
    check_case(
        "duplicate_manifest_key",
        {
            "p1/hua.package.json": '{"schema":"hua.package.v1","name":"p","version":"0.1.0"}\n',
            "p1/package.hua": "pub fn v() int { return 1 }\n",
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"app","version":"0.1.0","deps":{"p":"../p1","\\u0070":"../p1"}}\n',
            "app/main.hua": "import p\nfn main() {}\n",
        },
        "app/main.hua",
        "E5004",
    )

    # Case 8: Package-level manifest cycle -> E5002
    check_case(
        "package_manifest_cycle",
        {
            "a/hua.package.json": '{"schema":"hua.package.v1","name":"a","version":"0.1.0","deps":{"b":"../b"}}\n',
            "a/package.hua": "pub fn x() int { return 1 }\n",
            "b/hua.package.json": '{"schema":"hua.package.v1","name":"b","version":"0.1.0","deps":{"a":"../a"}}\n',
            "b/package.hua": "pub fn y() int { return 2 }\n",
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"app","version":"0.1.0","deps":{"a":"../a"}}\n',
            "app/main.hua": "import a\nfn main() { print(a.x()) }\n",
        },
        "app/main.hua",
        "E5002",
    )

    # Case 9: Module-level cross-package import cycle (via sub-modules in same package or acyclic manifest? Note manifest cycle is already E5002; intra-package cycle @pkg/a/package.hua -> @pkg/a/sub.hua -> @pkg/a/package.hua) -> E5002
    check_case(
        "intra_package_module_cycle",
        {
            "a/hua.package.json": '{"schema":"hua.package.v1","name":"a","version":"0.1.0"}\n',
            "a/package.hua": "import sub\npub fn x() int { return sub.y() }\n",
            "a/sub.hua": "import package\npub fn y() int { return 1 }\n",
            "app/hua.package.json": '{"schema":"hua.package.v1","name":"app","version":"0.1.0","deps":{"a":"../a"}}\n',
            "app/main.hua": "import a\nfn main() { print(a.x()) }\n",
        },
        "app/main.hua",
        "E5002",
    )

    return checks


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", required=True, type=Path, help="Path to huaBin SDK")
    parser.add_argument("--work", type=Path, default=None, help="Optional scratch directory outside repo")
    args = parser.parse_args()

    sdk = args.sdk.resolve()
    host = sdk / "bin/hua.exe"
    archive_exe = sdk / "lib/compiler/hua-archive.exe"

    temp_ctx = None
    if args.work is None:
        temp_ctx = tempfile.TemporaryDirectory(prefix="hua-step1-out-of-repo-")
        work = Path(temp_ctx.name).resolve()
    else:
        work = args.work.resolve()
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)

    try:
        cache_dir = work / "release-cache"
        env = clean_env({"HUA_RELEASE_CACHE": str(cache_dir)})

        src_tree = work / "workspace_src"
        entry = create_multi_package_workspace(src_tree)
        app_dir = entry.parent

        # Prepare test data in an independent runtime directory (completely separate from source!)
        run_dir = work / "独立部署 运行目录"
        run_dir.mkdir(parents=True)
        cfg_file = write_utf8(
            run_dir / "config.json",
            json.dumps({"title": "Prod Gateway", "min_level": 2, "allow_empty_service": False}, indent=2) + "\n",
        )
        good_jsonl = write_utf8(
            run_dir / "events.jsonl",
            "\n".join(
                [
                    '{"service": "Auth", "level": 3, "latency_ms": 420}',
                    '{"service": "billing", "level": 1, "latency_ms": 999}',
                    "",
                    '{"service": "  auth ", "level": 2, "latency_ms": 630}',
                    '{"service": "Billing", "level": 4, "latency_ms": 200}',
                ]
            )
            + "\n",
        )
        bad_jsonl = write_utf8(
            run_dir / "bad_events.jsonl",
            "\n".join(
                [
                    '{"service": "auth", "level": 2, "latency_ms": 15}',
                    '{"service": "   ", "level": 3, "latency_ms": 20}',
                ]
            )
            + "\n",
        )

        # Build artifacts:
        # 1. Host HUAB
        host_huab = run_dir / "audit_host.huab"
        r_b1 = run_cmd([host, "build", entry, "-o", host_huab], app_dir, env)
        assert r_b1.returncode == 0, r_b1.stderr

        # 2. Selfhost native archive HUAB
        sh_huab = run_dir / "audit_selfhost.huab"
        r_b2 = run_cmd([archive_exe, entry, sh_huab], app_dir, env | {"HUA_HOME": str(sdk)})
        assert r_b2.returncode == 0 and json.loads(r_b2.stdout)["status"] == "ok", (r_b2.stdout, r_b2.stderr)
        assert run_cmd([host, "check", sh_huab], run_dir, env).returncode == 0

        # 3. Release standalone EXE (cold + hot cache check)
        rel_exe = run_dir / "audit_cli.exe"
        r_rel_cold = run_cmd([host, "build", "--release", entry, "-o", rel_exe], app_dir, env)
        assert r_rel_cold.returncode == 0, r_rel_cold.stderr
        cold_receipt = json.loads(r_rel_cold.stdout)
        assert cold_receipt["generate_calls"] == 1 and cold_receipt["link_calls"] == 1, cold_receipt

        r_rel_hot = run_cmd([host, "build", "--release", entry, "-o", rel_exe], app_dir, env)
        assert r_rel_hot.returncode == 0, r_rel_hot.stderr
        hot_receipt = json.loads(r_rel_hot.stdout)
        assert (hot_receipt["generate_calls"], hot_receipt["compile_calls"], hot_receipt["link_calls"]) == (0, 0, 0), hot_receipt

        # Execute ALL paths on GOOD input while source still exists:
        cli_good_args = ["--config", str(cfg_file), "--input", str(good_jsonl)]
        res_vm = run_cmd([host, "run", entry, "--", *cli_good_args, "--output", str(run_dir / "out_vm.json")], run_dir, env)
        res_run_rel = run_cmd(
            [host, "run", "--release", entry, "--", *cli_good_args, "--output", str(run_dir / "out_run_rel.json")],
            run_dir,
            env,
        )
        assert res_vm.returncode == 0 and not res_vm.stderr, res_vm
        assert res_run_rel.returncode == 0 and not res_run_rel.stderr, res_run_rel
        expected_banner = "Prod Gateway: kept=3/4 latency=1,250ms (inits=1)\n"
        assert res_vm.stdout == expected_banner, res_vm.stdout
        assert res_run_rel.stdout == expected_banner, res_run_rel.stdout

        # Now RENAME / REMOVE the entire source workspace to prove relocated HUAB & EXE run without sources!
        hidden_src = work / "workspace_src_moved_away"
        src_tree.rename(hidden_src)
        assert not entry.exists()

        res_host_huab = run_cmd(
            [host, "run", host_huab, "--", *cli_good_args, "--output", str(run_dir / "out_hhuab.json")], run_dir, env
        )
        res_sh_huab = run_cmd(
            [host, "run", sh_huab, "--", *cli_good_args, "--output", str(run_dir / "out_shhuab.json")], run_dir, env
        )
        res_exe = run_cmd([rel_exe, *cli_good_args, "--output", str(run_dir / "out_exe.json")], run_dir, clean_env())

        for tag, r in [("host_huab", res_host_huab), ("sh_huab", res_sh_huab), ("rel_exe", res_exe)]:
            assert r.returncode == 0 and r.stdout == expected_banner and not r.stderr, (tag, r)

        # Verify all 5 output JSON files are byte-for-byte identical!
        expected_json = (run_dir / "out_vm.json").read_text(encoding="utf-8")
        parsed_out = json.loads(expected_json)
        assert parsed_out == {
            "kept_events": 3,
            "schema_inits": 1,
            "services": {"auth": 2, "billing": 1},
            "title": "Prod Gateway",
            "total_latency": 1250,
            "total_lines": 4,
        }, parsed_out
        for fname in ("out_run_rel.json", "out_hhuab.json", "out_shhuab.json", "out_exe.json"):
            assert (run_dir / fname).read_text(encoding="utf-8") == expected_json, fname

        # Restore source workspace to compare runtime error locations & exit codes across Source VM, HUAB, and relocated EXE!
        hidden_src.rename(src_tree)

        # Bad JSONL record (exit code 1, error inside @pkg/logschema/rules.hua propagated via @pkg/logschema/package.hua -> main.hua!)
        cli_bad_args = ["--strict", "--config", str(cfg_file), "--input", str(bad_jsonl)]
        err_vm = run_cmd([host, "run", entry, "--", *cli_bad_args], run_dir, env)
        src_tree.rename(hidden_src)
        err_hhuab = run_cmd([host, "run", host_huab, "--", *cli_bad_args], run_dir, env)
        err_shhuab = run_cmd([host, "run", sh_huab, "--", *cli_bad_args], run_dir, env)
        err_exe = run_cmd([rel_exe, *cli_bad_args], run_dir, clean_env())
        hidden_src.rename(src_tree)

        for tag, r in [("vm", err_vm), ("host_huab", err_hhuab), ("sh_huab", err_shhuab), ("exe", err_exe)]:
            assert r.returncode == 1 and not r.stdout, (tag, r)
            assert "logschema.record.empty_service: service cannot be empty" in r.stderr, (tag, r.stderr)
            assert "jsonl line 2" in r.stderr, (tag, r.stderr)

        # Check IDENTICAL stderr & exact @pkg/logschema/... and main.hua error locations across all 4!
        assert err_vm.stderr == err_hhuab.stderr == err_shhuab.stderr == err_exe.stderr, (
            err_vm.stderr,
            err_hhuab.stderr,
            err_shhuab.stderr,
            err_exe.stderr,
        )
        locs = extract_error_locations(err_exe.stderr)
        assert any(l.startswith("main.hua:") for l in locs), locs
        assert any(l.startswith("@pkg/logschema/rules.hua:") for l in locs), locs
        assert any(l.startswith("@pkg/logschema/package.hua:") for l in locs), locs

        # Usage error (--unknown flag -> hua.cli/usage -> exit code 2 across all 4!)
        u_vm = run_cmd([host, "run", entry, "--", "--unknown"], run_dir, env)
        u_shhuab = run_cmd([host, "run", sh_huab, "--", "--unknown"], run_dir, env)
        u_exe = run_cmd([rel_exe, "--unknown"], run_dir, clean_env())
        assert u_vm.returncode == 2 and u_shhuab.returncode == 2 and u_exe.returncode == 2, (u_vm, u_shhuab, u_exe)
        assert u_vm.stderr == u_shhuab.stderr == u_exe.stderr, (u_vm.stderr, u_exe.stderr)

        # Verify N3 cache invalidates when external package source (@pkg/logschema/rules.hua) is edited!
        rules_file = src_tree / "packages/logschema/rules.hua"
        orig_rules = rules_file.read_text(encoding="utf-8")
        write_utf8(rules_file, orig_rules + "\n# external package edit invalidation probe\n")
        r_after_pkg_edit = run_cmd([host, "build", "--release", entry, "-o", rel_exe], app_dir, env)
        assert r_after_pkg_edit.returncode == 0, r_after_pkg_edit.stderr
        edit_receipt = json.loads(r_after_pkg_edit.stdout)
        assert edit_receipt["generate_calls"] == 1, edit_receipt

        # Run full diagnostic parity matrix (9 error scenarios x 4 compiler/runtime commands = 36 checks)
        diag_checks = verify_diagnostics_parity(sdk, work, env)

        summary = {
            "status": "passed",
            "banner": expected_banner.strip(),
            "exe_bytes": rel_exe.stat().st_size,
            "exe_sha256": hashlib.sha256(rel_exe.read_bytes()).hexdigest(),
            "selfhost_huab_format": json.loads(r_b2.stdout)["format"],
            "error_locations": locs,
            "diagnostic_parity_checks": diag_checks,
        }
        print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)
    finally:
        if temp_ctx is not None:
            temp_ctx.cleanup()


if __name__ == "__main__":
    main()
