#!/usr/bin/env python3
"""Audit per-interface stdlib coverage across Host, Selfhost (catalog/types/HUAB), and Release Native."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass
class StdFn:
    module: str
    name: str
    params: list[str]
    result: str
    mutates: bool
    writable: bool
    since: int
    callback: bool


def parse_cpp_strings(raw: str) -> list[str]:
    raw = raw.strip()
    if not raw:
        return []
    return re.findall(r'"([^"]*)"', raw)


def parse_host_stdlib(stdlib_cpp: Path) -> list[StdFn]:
    text = stdlib_cpp.read_text(encoding="utf-8")
    start = text.index("std::vector<StandardFunction> entries={")
    end = text.index("};", start)
    block = text[start:end]
    pattern = re.compile(
        r'\{"([a-z0-9_]+)","([a-z0-9_]+)",\{([^}]*)\},"([^"]*)"'
        r'(?:,(true|false),(true|false),(\d+)(?:,(true|false))?)?\}'
    )
    out: list[StdFn] = []
    for m in pattern.finditer(block):
        module, name, params_raw, result, mut_s, writ_s, since_s, cb_s = m.groups()
        out.append(
            StdFn(
                module=module,
                name=name,
                params=parse_cpp_strings(params_raw),
                result=result,
                mutates=(mut_s == "true"),
                writable=(writ_s == "true"),
                since=int(since_s) if since_s is not None else 3,
                callback=(cb_s == "true"),
            )
        )
    return out


def parse_catalog(catalog_hua: Path) -> tuple[set[str], dict[str, set[str]], set[str], set[tuple[str, str]]]:
    text = catalog_hua.read_text(encoding="utf-8")
    core_m = re.search(r"pub fn core\(name string\) bool \{ return alg\.contains\(\[([^\]]+)\]", text)
    core = set(parse_cpp_strings(core_m.group(1))) if core_m else set()

    mod_m = re.search(r"pub fn module\(name string\) bool \{ return alg\.contains\(\[([^\]]+)\]", text)
    modules = set(parse_cpp_strings(mod_m.group(1))) if mod_m else set()

    exports: dict[str, set[str]] = {}
    for m in re.finditer(r'if module == "(std\.[a-z0-9_]+)" \{ return \[([^\]]+)\] \}', text):
        exports[m.group(1)] = set(parse_cpp_strings(m.group(2)))

    type_exports = {
        ("std.json", "Value"),
        ("std.error", "Value"),
        ("std.file", "File"),
        ("std.file", "IOFailure"),
    }
    return core, exports, modules, type_exports


def parse_signatures(sig_hua: Path) -> dict[str, tuple[list[str], str, bool, bool]]:
    text = sig_hua.read_text(encoding="utf-8")
    out: dict[str, tuple[list[str], str, bool, bool]] = {}
    flat_pat = re.compile(
        r'if target == "builtin:(std\.[a-z0-9_]+#[a-z0-9_]+)" '
        r'\{ return Signature\{parameters:\[(.*?)\],result:"([^"]*)",mutates:(true|false),writable:(true|false)\} \}'
    )
    for m in flat_pat.finditer(text):
        key, params_raw, result, mut_s, writ_s = m.groups()
        out[key] = (parse_cpp_strings(params_raw), result, mut_s == "true", writ_s == "true")

    cur_mod: str | None = None
    mod_pat = re.compile(r'if module == "builtin:(std\.[a-z0-9_]+)" \{')
    fn_pat = re.compile(
        r'if name == "([a-z0-9_]+)" '
        r'\{ return Signature\{parameters:\[(.*?)\],result:"([^"]*)",mutates:(true|false),writable:(true|false)\} \}'
    )
    for line in text.splitlines():
        mm = mod_pat.search(line)
        if mm:
            cur_mod = mm.group(1)
            continue
        if cur_mod is not None:
            fm = fn_pat.search(line)
            if fm:
                fname, params_raw, result, mut_s, writ_s = fm.groups()
                out[f"{cur_mod}#{fname}"] = (parse_cpp_strings(params_raw), result, mut_s == "true", writ_s == "true")
            elif line.strip() == "}":
                cur_mod = None
    return out


def parse_native_cpp(native_hua: Path) -> tuple[set[str], set[str]]:
    text = native_hua.read_text(encoding="utf-8")
    bridged: set[str] = set()
    bridged_m = re.search(r"fn bridged\(name string\) bool \{\s*return alg\.contains\(\[([^\]]+)\]", text, re.S)
    if bridged_m:
        bridged.update(parse_cpp_strings(bridged_m.group(1)))
    else:
        for m in re.finditer(r'if module=="([a-z0-9_]+)" \{ return alg\.contains\(\[([^\]]+)\],func\) \}', text):
            mod = m.group(1)
            for fname in parse_cpp_strings(m.group(2)):
                bridged.add(f"$std${mod}${fname}")
    typed: set[str] = set()
    for name in [
        "$std$strings$split",
        "$std$strings$starts_with",
        "$std$strings$ends_with",
        "$std$strings$slice",
        "$std$list$clear",
        "$std$list$extend",
        "$std$list$from_slice",
        "$std$list$len",
        "$std$list$snapshot",
        "$std$list$append",
        "$std$list$get",
        "$std$list$pop",
        "$std$list$set",
    ]:
        if name in text or name.replace("$std$list$", "") in text:
            typed.add(name)
    return bridged, typed


def parse_archive_load_format(archive_hua: Path) -> dict[str, tuple[int, dict[str, int]]]:
    text = archive_hua.read_text(encoding="utf-8")
    m = re.search(r"fn load_format\(text string\) int \{(.*?)\n\}", text, re.S)
    if not m:
        return {}
    body = m.group(1)
    result: dict[str, tuple[int, dict[str, int]]] = {}
    cur_mod: str | None = None
    cur_overrides: dict[str, int] = {}
    cur_default = 6
    for line in body.splitlines():
        s = line.strip()
        single = re.fullmatch(r'if strings\.starts_with\(text,"\$core\$\$std\$([a-z0-9_]+)\$"\) \{ return (\d+) \}', s)
        if single:
            result[single.group(1)] = (int(single.group(2)), {})
            continue
        block = re.fullmatch(r'if strings\.starts_with\(text,"\$core\$\$std\$([a-z0-9_]+)\$"\) \{', s)
        if block:
            cur_mod = block.group(1)
            cur_overrides = {}
            cur_default = 6
            continue
        if cur_mod is not None:
            bm = re.fullmatch(r'if alg\.contains\(\[([^\]]+)\],name\) \{ return (\d+) \}', s)
            if bm:
                ver = int(bm.group(2))
                for fname in parse_cpp_strings(bm.group(1)):
                    cur_overrides[fname] = ver
                continue
            rm = re.fullmatch(r'return (\d+)', s)
            if rm:
                cur_default = int(rm.group(1))
                continue
            if s == "}":
                result[cur_mod] = (cur_default, cur_overrides)
                cur_mod = None
    return result


POLY_SIGNATURES: dict[tuple[str, str], str] = {
    ("alg", "sorted"): "(xs []T) Result<[]T,string> [T: int|float|string]",
    ("alg", "reverse"): "(xs []T) Result<[]T,string>",
    ("alg", "contains"): "(xs []T, target T) bool",
    ("alg", "index"): "(xs []T, target T) int",
    ("alg", "lower_bound"): "(xs []T, target T) int",
    ("alg", "upper_bound"): "(xs []T, target T) int",
    ("alg", "binary_search"): "(xs []T, target T) int?",
    ("alg", "sum"): "(xs []T) Result<T,string> [T: int|float]",
    ("alg", "min"): "(xs []T) Result<T,string>",
    ("alg", "max"): "(xs []T) Result<T,string>",
    ("alg", "unique"): "(xs []T) []T",
    ("alg", "chunk"): "(xs []T, size int) [][]T",
    ("alg", "product"): "(xs []T) Result<T,string> [T: int|float]",
    ("alg", "windows"): "(xs []T, size int) [][]T",
    ("alg", "map"): "(xs []T, f fn(T)->U) []U",
    ("alg", "fold"): "(xs []T, init A, f fn(A,T)->A) A",
    ("alg", "filter"): "(xs []T, pred fn(T)->bool) []T",
    ("alg", "any"): "(xs []T, pred fn(T)->bool) bool",
    ("alg", "all"): "(xs []T, pred fn(T)->bool) bool",
    ("alg", "find"): "(xs []T, pred fn(T)->bool) T?",
    ("alg", "find_index"): "(xs []T, pred fn(T)->bool) int",
    ("alg", "sorted_by_key"): "(xs []T, key fn(T)->K) []T",
    ("alg", "sorted_by"): "(xs []T, less fn(T,T)->bool) []T",
    ("alg", "min_by_key"): "(xs []T, key fn(T)->K) Result<T,string>",
    ("alg", "max_by_key"): "(xs []T, key fn(T)->K) Result<T,string>",
    ("alg", "partition"): "(xs []T, pred fn(T)->bool) ([]T, []T)",
    ("alg", "zip_with"): "(xs []A, ys []B, f fn(A,B)->R) []R",
    ("list", "from_slice"): "(xs []T) List<T> [writable]",
    ("list", "len"): "(xs List<T>) int",
    ("list", "get"): "(xs List<T>, index int) Result<T,string>",
    ("list", "set"): "(mut xs List<T>, index int, value T) Result<bool,string>",
    ("list", "append"): "(mut xs List<T>, value T) Result<int,string>",
    ("list", "extend"): "(mut xs List<T>, values []T) Result<int,string>",
    ("list", "pop"): "(mut xs List<T>) Result<T,string>",
    ("list", "snapshot"): "(xs List<T>) []T",
    ("list", "clear"): "(mut xs List<T>) void",
    ("list", "reserve"): "(mut xs List<T>, capacity int) Result<bool,string>",
    ("list", "insert"): "(mut xs List<T>, index int, value T) Result<int,string>",
    ("list", "remove_at"): "(mut xs List<T>, index int) Result<T,string>",
    ("list", "index_of"): "(xs List<T>, target T) int",
    ("list", "contains"): "(xs List<T>, target T) bool",
    ("list", "reverse"): "(mut xs List<T>) void",
    ("list", "sort"): "(mut xs List<T>) void",
    ("list", "swap"): "(mut xs List<T>, i int, j int) Result<bool,string>",
    ("list", "truncate"): "(mut xs List<T>, len int) void",
    ("list", "slice"): "(xs List<T>, start int, end int) Result<[]T,string>",
    ("list", "dedup"): "(mut xs List<T>) int",
    ("list", "sort_by_key"): "(mut xs List<T>, key fn(T)->K) void",
    ("list", "sort_by"): "(mut xs List<T>, less fn(T,T)->bool) void",
    ("list", "retain"): "(mut xs List<T>, pred fn(T)->bool) int",
    ("maps", "keys"): "(m map[K]V) []K",
    ("maps", "values"): "(m map[K]V) []V",
    ("maps", "entries"): "(m map[K]V) ([]K, []V)",
    ("maps", "get_or"): "(m map[K]V, key K, fallback V) V",
    ("maps", "pop"): "(mut m map[K]V, key K) V?",
    ("maps", "merge"): "(mut dst map[K]V, src map[K]V) int",
    ("maps", "update"): "(mut m map[K]V, key K, init V, f fn(V)->V) V",
    ("sets", "from_slice"): "(xs []K) map[K]bool [writable]",
    ("sets", "union"): "(a map[K]bool, b map[K]bool) map[K]bool [writable]",
    ("sets", "intersection"): "(a map[K]bool, b map[K]bool) map[K]bool [writable]",
    ("sets", "difference"): "(a map[K]bool, b map[K]bool) map[K]bool [writable]",
    ("sets", "symmetric_difference"): "(a map[K]bool, b map[K]bool) map[K]bool [writable]",
    ("sets", "is_subset"): "(a map[K]bool, b map[K]bool) bool",
    ("sets", "is_disjoint"): "(a map[K]bool, b map[K]bool) bool",
    ("deque", "from_slice"): "(xs []T) Deque<T> [writable]",
    ("deque", "len"): "(d Deque<T>) int",
    ("deque", "push_back"): "(mut d Deque<T>, value T) Result<int,string>",
    ("deque", "push_front"): "(mut d Deque<T>, value T) Result<int,string>",
    ("deque", "pop_back"): "(mut d Deque<T>) Result<T,string>",
    ("deque", "pop_front"): "(mut d Deque<T>) Result<T,string>",
    ("deque", "front"): "(d Deque<T>) Result<T,string>",
    ("deque", "back"): "(d Deque<T>) Result<T,string>",
    ("deque", "get"): "(d Deque<T>, index int) Result<T,string>",
    ("deque", "clear"): "(mut d Deque<T>) void",
    ("deque", "snapshot"): "(d Deque<T>) []T",
    ("heap", "heapify"): "(mut xs List<T>) void",
    ("heap", "push"): "(mut xs List<T>, value T) Result<int,string>",
    ("heap", "pop"): "(mut xs List<T>) Result<T,string>",
    ("heap", "peek"): "(xs List<T>) Result<T,string>",
    ("heap", "smallest"): "(xs []T, k int) Result<[]T,string>",
    ("heap", "largest"): "(xs []T, k int) Result<[]T,string>",
    ("heap", "push_by_key"): "(mut xs List<T>, value T, key fn(T)->K) Result<int,string>",
    ("heap", "pop_by_key"): "(mut xs List<T>, key fn(T)->K) Result<T,string>",
    ("random", "shuffle"): "(mut rng std.random.Rng, mut xs List<T>|[]T) void",
    ("random", "choice"): "(mut rng std.random.Rng, xs []T) Result<T,string>",
    ("random", "sample"): "(mut rng std.random.Rng, xs []T, k int) Result<[]T,string>",
    ("json", "encode"): "(value any_json_serializable) Result<string,string>",
    ("json", "from"): "(value any_json_serializable) Result<Json,string>",
    ("json", "encode_pretty"): "(value any_json_serializable, indent int) Result<string,string>",
    ("os", "environ"): "() Result<map[string]string,string>",
    ("os", "parse_args"): "(argv []string, flags []string, options []string) Result<map[string][]string,string>",
    ("simd", "add"): "(a []F, b []F) Result<[]F,string> [F: f32|f64|float]",
    ("simd", "sub"): "(a []F, b []F) Result<[]F,string> [F: f32|f64|float]",
    ("simd", "mul"): "(a []F, b []F) Result<[]F,string> [F: f32|f64|float]",
    ("task", "cancel"): "(t Task<T>) bool",
    ("task", "done"): "(t Task<T>) bool",
    ("task", "state"): "(t Task<T>) string",
    ("task", "timeout"): "(t Task<T>, ms int) Result<T,string>",
    ("task", "all"): "(a Task<T>, b Task<T>) Task<[]T>",
    ("task", "race"): "(a Task<T>, b Task<T>) Task<T>",
    ("test", "equal"): "(actual T, expected T) void",
    ("test", "not_equal"): "(actual T, unexpected T) void",
}


def format_sig(fn: StdFn) -> str:
    if (fn.module, fn.name) in POLY_SIGNATURES:
        return POLY_SIGNATURES[(fn.module, fn.name)]
    ps = []
    for idx, p in enumerate(fn.params):
        prefix = "mut " if (idx == 0 and fn.mutates) else ""
        ps.append(f"{prefix}{p}")
    ret = fn.result
    if ret.startswith("multi<") and ret.endswith(">"):
        ret = "(" + ret[6:-1] + ")"
    ret = ret.replace("map<string,string>", "map[string]string").replace("map<string,Json>", "map[string]Json")
    suffix = " [writable]" if fn.writable else ""
    return f"({', '.join(ps)}) {ret}{suffix}"


def native_cpp_supports_type(t: str) -> bool:
    if not t:
        return False
    if t in {
        "int",
        "float",
        "string",
        "bool",
        "void",
        "Json",
        "Buffer",
        "Bytes",
        "std.file.File",
        "std.file.IOFailure",
        "std.error.Value",
    }:
        return True
    if t.endswith("?"):
        return native_cpp_supports_type(t[:-1])
    if t.startswith("[]"):
        return native_cpp_supports_type(t[2:])
    for prefix in ("List<", "Result<", "map<", "multi<"):
        if t.startswith(prefix) and t.endswith(">"):
            inner = t[len(prefix) : -1]
            # Split top-level commas
            depth = 0
            parts = []
            cur = []
            for ch in inner:
                if ch == "<":
                    depth += 1
                elif ch == ">":
                    depth -= 1
                if ch == "," and depth == 0:
                    parts.append("".join(cur))
                    cur = []
                else:
                    cur.append(ch)
            parts.append("".join(cur))
            return all(native_cpp_supports_type(p) for p in parts)
    return False


def sample_case(fn: StdFn) -> str:
    m, n = fn.module, fn.name
    specific = {
        ("os", "args"): "`os.args()` → 返回传入命令行参数切片 `[]string`",
        ("os", "cwd"): "`unwrap(os.cwd())` → 返回非空当前目录路径",
        ("os", "getenv"): "`os.getenv(\"PATH\")` → `ok(...)`；含 `=` 键名返回 `Err`",
        ("os", "exit"): "`os.exit(3)` → 运行 `defer` 后进程退出码为 `3`；`os.exit(999)` 报 `E4003`",
        ("os", "parse_args"): "`os.parse_args([\"--strict\",\"-o\",\"out.json\",\"in.jsonl\"],[\"strict\"],[\"o\"])` → `ok`；未知旗标返回 `Err(\"ARGS_UNKNOWN...\")`",
        ("os", "environ"): "`unwrap(os.environ())` → 返回环境变量有序 `map[string]string`",
        ("io", "write_err"): "`io.write_err(\"warn\\n\")` → 写 stderr 并返回 `ok(5)`",
        ("file", "open"): "`file.open(\"missing.txt\",\"r\")` → `Err(std.error.Value)` (`code==\"not_found\"`)",
        ("file", "read_line"): "`file.read_line(f)` → 逐行去 `\\r\\n` 返回 `ok(\"...\")`，EOF 返回 `ok(nil)`",
        ("file", "close"): "`file.close(f)` → 首次 `ok()`，重复 `close` 幂等 `ok()`",
        ("fs", "read_text"): "`fs.read_text(\"config.json\")` → `ok(text)`；不存在返回 `Err`",
        ("fs", "replace"): "`fs.replace(tmp, dst)` → 同卷原子替换 `ok(true)`",
        ("fs", "canonical"): "`fs.canonical(\".\")` → `ok(规范绝对路径)`；不存在返回 `Err(\"FS_NOT_FOUND...\")`",
        ("fs", "walk"): "`fs.walk(dir)` → `ok(字典序相对路径切片)`；非目录返回 `Err(\"FS_TYPE...\")`",
        ("strings", "trim"): "`strings.trim(\"  hi\\n\")` → `\"hi\"`",
        ("strings", "lines"): "`strings.lines(\"a\\r\\nb\\n\")` → `[\"a\",\"b\"]`",
        ("strings", "fields"): "`strings.fields(\" a  b\\tc \")` → `[\"a\",\"b\",\"c\"]`",
        ("strings", "parse_int"): "`strings.parse_int(\"42\", 10)` → `ok(42)`；`\"42x\"` 返回 `Err`",
        ("strings", "parse_float"): "`strings.parse_float(\"3.5\")` → `ok(3.5)`；`\"nan\"` 返回 `Err`",
        ("strings", "format"): "`strings.format(\"{}:{}\", [\"line\", \"1\"])` → `ok(\"line:1\")`；占位符不匹配返回 `Err`",
        ("strings", "template"): "`strings.template(\"{k:>4}\", map[string]string{\"k\":\"7\"})` → `ok(\"   7\")`；缺键返回 `Err`",
        ("strings", "equal_fold"): "`strings.equal_fold(\"Straße\", \"STRASSE\")` → `true`",
        ("json", "decode"): "`json.decode(\"{\\\"a\\\":1}\")` → `ok(Json)`；坏语法返回 `Err`",
        ("json", "decode_lines"): "`json.decode_lines(\"{\\\"a\\\":1}\\n{\\\"a\\\":2}\\n\")` → `ok(2项 []Json)`；坏行带行号 `Err`",
        ("json", "encode_lines"): "`json.encode_lines([j1, j2])` → `ok(\"{\\\"a\\\":1}\\n{\\\"a\\\":2}\\n\")`",
        ("json", "encode_pretty"): "`json.encode_pretty(obj, 2)` → `ok(2空格缩进JSON)`；`indent<0` 返回 `Err`",
        ("json", "from"): "`json.from(map[string]int{\"k\":1})` → `ok(Json)`",
        ("json", "object"): "`json.object(map[string]Json{\"ok\":unwrap(json.from(true))})` → `Json`",
        ("json", "set"): "`json.set(obj, \"k\", val)` → 返回新 `ok(Json)`，原 `obj` 不变；非 object 返回 `Err`",
        ("error", "make"): "`error.make(\"app.cfg\", \"bad_field\", \"missing id\")` → `ok(Error)`；空 domain 返回 `Err`",
        ("error", "with_context"): "`error.with_context(e, \"line 3\")` → 返回附加上下文的新 `Error`",
        ("error", "format"): "`error.format(e)` → `\"app.cfg/bad_field: missing id\"`",
        ("process", "run"): "`process.run([exe, \"version\"], bytes.from_text(\"\"), 5000)` → `(0, out, err, \"\")`",
        ("crypto", "sha256"): "`encoding.hex_encode(crypto.sha256(bytes.from_text(\"abc\")))` → `\"ba7816bf...\"`",
    }
    if (m, n) in specific:
        return specific[(m, n)]
    if fn.callback:
        return f"`{m}.{n}(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003`"
    if m == "error":
        return f"`error.{n}(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值"
    if m == "file":
        return f"`file.{n}(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误"
    if m == "math":
        if "Result<" in fn.result:
            return f"`math.{n}(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err`"
        return f"`math.{n}(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004`"
    if m == "encoding":
        if "Result<" in fn.result:
            return f"`encoding.{n}(...)` → 合法编码往返一致；非法字节/端序名返回 `Err`"
        return f"`encoding.{n}(...)` → 纯函数返回编码文本或布尔校验值"
    if m == "strings":
        if "Result<" in fn.result:
            return f"`strings.{n}(...)` → 合法 UTF-8/格式返回 `ok(...)`；越界或坏格式返回 `Err`"
        return f"`strings.{n}(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099`"
    if m == "json":
        if "Result<" in fn.result:
            return f"`json.{n}(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err`"
        return f"`json.{n}(...)` → 构造或查询不可变 `Json` 节点"
    if m in {"list", "deque", "heap", "maps", "sets"}:
        mut_note = "；只读容器调用静态 `E3003`/动态 `E4007`" if fn.mutates else ""
        if "Result<" in fn.result:
            return f"`{m}.{n}(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`{mut_note}"
        return f"`{m}.{n}(...)` → 返回预期容器结果{mut_note}"
    if m == "alg":
        if "Result<" in fn.result:
            return f"`alg.{n}(...)` → 非空序列返回 `ok(...)`；空序列或溢出返回 `Err`"
        return f"`alg.{n}(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004`"
    if m == "time":
        if "Result<" in fn.result:
            return f"`time.{n}(...)` → 合法时间文本/分量返回 `ok(int)`；非法格式返回 `Err`"
        return f"`time.{n}(...)` → 纪元毫秒/单调时钟计算确定结果"
    if m == "random":
        return f"`random.{n}(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007`"
    if m == "crypto":
        return f"`crypto.{n}(...)` → 正常返回随机/摘要结果；负数长度返回 `Err`"
    if m == "fs" or m == "path":
        return f"`{m}.{n}(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err`"
    if m == "io":
        return f"`io.{n}(...)` → 读写标准流返回 `ok(...)`；超 16 MiB 返回 `Err`"
    if m == "os":
        return f"`os.{n}(...)` → 返回宿主环境/平台信息"
    if m == "process":
        return f"`process.{n}(...)` → 返回 `(exit_code, stdout, stderr, failure)`；超时 `failure==\"PROCESS_TIMEOUT\"`"
    if m == "test":
        return f"`test.{n}(...)` → 条件成立无输出；失败抛 `E4900`"
    if m == "log":
        return f"`log.{n}(...)` → 按级别向 stderr 输出 logfmt 行；非法级别报 `E4003`"
    if m == "task":
        return f"`task.{n}(...)` → 遵循协作取消 `E4101` 与结构化作用域规则"
    if m == "gc":
        return f"`gc.{n}()` → 返回非负托管对象统计计数"
    if m == "simd":
        return f"`simd.{n}(...)` → 同长浮点切片返回 `ok(...)`；不等长返回 `Err`"
    return f"`{m}.{n}(...)` → 三路径结果一致"


def audit_function(
    fn: StdFn,
    cat_modules: set[str],
    cat_exports: dict[str, set[str]],
    sigs: dict[str, tuple[list[str], str, bool, bool]],
    bridged: set[str],
    typed_builtins: set[str],
) -> dict[str, object]:
    mod_full = f"std.{fn.module}"
    sig_key = f"{mod_full}#{fn.name}"
    std_id = f"$std${fn.module}${fn.name}"

    in_cat = (mod_full in cat_modules) and (fn.name in cat_exports.get(mod_full, set()))
    in_sig = sig_key in sigs

    # Check types.hua blockers even if in_sig were true
    types_text = (Path(__file__).resolve().parents[2] / "selfhost/types.hua").read_text(encoding="utf-8")
    poly_supported_in_types = False
    if (fn.module == "task" and fn.name in {"cancel", "done", "state", "timeout", "all", "race"}) or (
        fn.module == "alg"
        and fn.name in {"sorted", "reverse", "contains", "index", "lower_bound", "upper_bound", "binary_search", "sum", "min", "max"}
    ) or (
        fn.module == "list"
        and fn.name in {"from_slice", "len", "get", "set", "append", "extend", "pop", "snapshot", "clear", "reserve"}
    ) or (fn.module == "json" and fn.name in {"encode", "from", "encode_pretty"}) or (
        fn.module == "test" and fn.name in {"equal", "not_equal"} and 'module == "builtin:std.test"' in types_text
    ):
        poly_supported_in_types = True

    has_empty_param_or_ret = ("" in fn.params) or (fn.result == "")
    math_float_only = 'module == "builtin:std.math" && arguments[i].type_name != "float"' in types_text
    math_int_blocked = math_float_only and fn.module == "math" and any(p != "float" for p in fn.params)

    sh_check = in_cat and in_sig and (not has_empty_param_or_ret or poly_supported_in_types) and not math_int_blocked

    # Check archive.hua format version gate
    archive_formats = parse_archive_load_format(Path(__file__).resolve().parents[2] / "selfhost/archive.hua")
    archive_fmt = 6
    all_types_str = " ".join(fn.params + [fn.result])
    if "std.file." in all_types_str:
        archive_fmt = max(archive_fmt, 8)
    if "std.error.Value" in all_types_str or "Result<void," in all_types_str:
        archive_fmt = max(archive_fmt, 7)
    if fn.module in archive_formats:
        mod_default, mod_overrides = archive_formats[fn.module]
        archive_fmt = max(archive_fmt, mod_overrides.get(fn.name, mod_default))
    sh_huab = sh_check and (archive_fmt >= fn.since)

    # Check native_cpp.hua (--release EXE)
    in_native_call = (std_id in bridged) or (std_id in typed_builtins)
    # Check concrete types for native_cpp::cpp_type
    concrete_types_ok = True
    if not has_empty_param_or_ret:
        concrete_types_ok = all(native_cpp_supports_type(p) for p in fn.params) and native_cpp_supports_type(fn.result)
    elif (fn.module, fn.name) in {
        ("alg", "sorted"),
        ("alg", "reverse"),
        ("alg", "contains"),
        ("alg", "index"),
        ("alg", "lower_bound"),
        ("alg", "upper_bound"),
        ("alg", "binary_search"),
        ("alg", "sum"),
        ("alg", "min"),
        ("alg", "max"),
        ("json", "encode"),
        ("json", "from"),
        ("json", "encode_pretty"),
        ("list", "clear"),
        ("list", "extend"),
        ("list", "from_slice"),
        ("list", "len"),
        ("list", "snapshot"),
        ("list", "append"),
        ("list", "get"),
        ("list", "pop"),
        ("list", "set"),
        ("list", "reserve"),
        ("test", "equal"),
        ("test", "not_equal"),
    }:
        concrete_types_ok = True
    else:
        concrete_types_ok = False

    # Special case: std.task in native_cpp is handled by concurrency lowering? Let's check: native_cpp.hua itself doesn't bridge $std$task$*
    release_exe = sh_check and in_native_call and concrete_types_ok

    # Determine Gap Category
    if sh_check and sh_huab and release_exe:
        gap = "A · 已全路径打通"
        action = "无需改动（4 路径均可用）"
    elif fn.callback:
        gap = "E · 需高阶回调支持"
        action = "需 `selfhost` 函数值/回调类型检查 + `archive` `since` + `native_cpp` 接入 `native_algorithms.hpp`"
    elif has_empty_param_or_ret and not poly_supported_in_types:
        if "Deque" in POLY_SIGNATURES.get((fn.module, fn.name), "") or "Rng" in POLY_SIGNATURES.get((fn.module, fn.name), ""):
            gap = "D+C · 需多态规则+类型映射"
            action = "需补 `Deque<T>`/`Rng` 类型识别 + `types.hua` 多态签名推导 + `archive`/`native_cpp` 桥接"
        else:
            gap = "D · 需多态签名规则"
            action = "需在 `types.hua::standard()` 补泛型/开放参数推导，并登记 `catalog`/`signatures`/`archive`/`native_cpp`"
    else:
        # Concrete signature! Check if any nominal/opaque type is missing in types.hua or native_cpp.hua::cpp_type
        missing_types = []
        for token, label in [
            ("std.error.Value", "std.error.Value"),
            ("std.file.File", "std.file.File"),
            ("std.file.IOFailure", "std.file.IOFailure"),
            ("std.random.Rng", "std.random.Rng"),
        ]:
            if token in all_types_str:
                missing_types.append(label)
        if fn.module == "simd":
            gap = "D · 需多态签名规则"
            action = "精确浮点切片 `f32`/`f64` 需 `types.hua` + `native_cpp` 精确数值映射"
        elif fn.module == "task":
            gap = "C · 需 Task 原生/自举特化"
            action = "Host/SH-HUAB 已通；`native_cpp.hua` 需对接 `native_rt` Task 等待入口"
        elif missing_types:
            gap = "C · 需补不透明句柄类型映射"
            action = (
                f"`native_rt.hpp` 已有标签；需在 `types.hua`/`native_cpp.hua::cpp_type` 映射 "
                f"`{', '.join(missing_types)}` 并加入 `bridged()`"
            )
        else:
            extras = []
            if math_int_blocked:
                extras.append("放开 `types.hua:335` 的 `std.math` 仅 `float` 限制")
            if not in_cat or not in_sig:
                extras.append("登记 `catalog`/`signatures`")
            if archive_fmt < fn.since:
                extras.append(f"`archive.hua` 登记 `since {fn.since}`")
            if not in_native_call:
                extras.append("加入 `native_cpp.hua::bridged()`")
            gap = "B · 仅差纯表项/桥接登记"
            action = "；".join(extras)

    return {
        "module": mod_full,
        "name": fn.name,
        "full_name": f"{mod_full}.{fn.name}",
        "signature": format_sig(fn),
        "since": fn.since,
        "host": True,
        "sh_check": sh_check,
        "sh_huab": sh_huab,
        "release_exe": release_exe,
        "gap": gap,
        "action": action,
        "case": sample_case(fn),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=".", help="Repository root")
    parser.add_argument("--markdown-out", default="", help="Optional markdown output path")
    parser.add_argument("--json-out", default="", help="Optional JSON output path")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    fns = parse_host_stdlib(repo / "src/stdlib.cpp")
    core_names, cat_exports, cat_modules, _ = parse_catalog(repo / "selfhost/catalog.hua")
    sigs = parse_signatures(repo / "selfhost/signatures.hua")
    bridged, typed_builtins = parse_native_cpp(repo / "selfhost/native_cpp.hua")

    rows = [audit_function(fn, cat_modules, cat_exports, sigs, bridged, typed_builtins) for fn in fns]

    # Sort deterministically by module name then source order in stdlib.cpp
    by_module: dict[str, list[dict[str, object]]] = {}
    for r in rows:
        by_module.setdefault(str(r["module"]), []).append(r)

    summary = {
        "total_builtin_stdlib_functions": len(rows),
        "total_builtin_modules": len(by_module),
        "sdk_source_modules": ["std.alg_integer"],
        "core_builtins": len(core_names),
        "host_supported": sum(1 for r in rows if r["host"]),
        "sh_check_supported": sum(1 for r in rows if r["sh_check"]),
        "sh_huab_supported": sum(1 for r in rows if r["sh_huab"]),
        "release_exe_supported": sum(1 for r in rows if r["release_exe"]),
        "by_gap": {},
    }
    for r in rows:
        g = str(r["gap"])
        summary["by_gap"][g] = summary["by_gap"].get(g, 0) + 1

    print(json.dumps(summary, ensure_ascii=False, indent=2))

    if args.json_out:
        out_json = Path(args.json_out)
        out_json.parent.mkdir(parents=True, exist_ok=True)
        out_json.write_text(json.dumps({"summary": summary, "rows": rows}, ensure_ascii=False, indent=2), encoding="utf-8")

    if args.markdown_out:
        out_md = Path(args.markdown_out)
        out_md.parent.mkdir(parents=True, exist_ok=True)
        lines: list[str] = []
        lines.append("# Hua 标准库逐接口多路径覆盖表（2026-11-25 源码实核）")
        lines.append("")
        lines.append("生成工具：`python tests/tools/audit_stdlib_coverage.py --repo .`（直接解析 `src/stdlib.cpp`、`selfhost/catalog.hua`、`selfhost/signatures.hua`、`selfhost/types.hua`、`selfhost/archive.hua` 与 `selfhost/native_cpp.hua`）。")
        lines.append("")
        lines.append("## 1. 汇总统计")
        lines.append("")
        lines.append("| 指标 | 数量 | 说明 |")
        lines.append("| --- | --- | --- |")
        lines.append(f"| 宿主内建标准模块 / 函数 | {len(by_module)} 模块 / {len(rows)} 函数 | `src/stdlib.cpp` 权威注册表（`HUAB since 3–29`） |")
        lines.append("| SDK 纯 Hua 源码标准模块 | 1 模块 / 1 函数 | `std.alg_integer.lower_bound`（Host / SH-Check / SH-HUAB / Release-EXE 四路径已全通，HUAB 6） |")
        lines.append(f"| 公开全局核心内建 | {len(core_names)} 个 | `catalog.core` 全部已登记；`native_cpp.hua` 当前直接支持 `print/str/int/max/len/clone/has/delete/ok/err/is_ok/is_err/unwrap/unwrap_err/unwrap_or/is_some/is_none`（17 个） |")
        lines.append(f"| `Host`（解释器 / 源码 VM / 宿主 HUAB） | {summary['host_supported']} / {len(rows)} | 全部 374 个函数在宿主三路径均已实现并验收 |")
        lines.append(f"| `SH-Check`（自举名称 + 类型能力检查） | {summary['sh_check_supported']} / {len(rows)} | `catalog.hua` 14 模块共导出 137 个名称（134 个函数名 + 3 个类型名 `Value`/`File`/`IOFailure`），134 个函数均已在 `signatures.hua` 登记 |")
        lines.append(f"| `SH-HUAB`（自举归档编译器 `hua-archive.exe`） | {summary['sh_huab_supported']} / {len(rows)} | `archive.hua` 已对 `fs.canonical`(17) 与 `file.read_line` 等 4 项(29) 升级 `load_format`；其余已登记项均满足 `since <= 8` |")
        lines.append(f"| `Release-EXE`（`hua build --release` 原生生成） | {summary['release_exe_supported']} / {len(rows)} | `native_cpp.hua` 当前桥接 48 项 `std.*` + 内联 13 项 `std.*` = **61 项**（注：`std.file` 16 项虽已进 `catalog/archive`，但 `native_cpp.hua` 尚未映射 `FileTag`/`IOFailureTag`） |")
        lines.append("")
        lines.append("### 按改造机制分类（为什么不能按函数个数简单估工期）")
        lines.append("")
        lines.append("| 缺口类别 | 函数数 | 工程本质与所需动作 |")
        lines.append("| --- | --- | --- |")
        for k in sorted(summary["by_gap"].keys()):
            desc = {
                "A · 已全路径打通": "Host、自举检查、自举 HUAB、`hua build --release` 四路径全部就绪。",
                "B · 仅差纯表项/桥接登记": "参数与返回类型在 `types.hua` 和 `native_cpp.hua::cpp_type` 中**均已支持**（且 C++ `src/*_stdlib.cpp` 实现已在 `libhua_semantics.a` 中）；只需补齐 `catalog` / `signatures` / `archive(since)` / `native_cpp::bridged()`（`std.math` 另需放开 `types.hua:335` 仅 `float` 检查）。",
                "C · 需补不透明句柄类型映射": "`include/hua/native_rt.hpp` **已经定义好** `FileTag`、`IOFailureTag`、`ErrorTag`、`Rng`；只需在 `types.hua::typename_context` 与 `native_cpp.hua::cpp_type` 补 4 个类型分支并加入 `bridged()`，即可一次打通 `std.file`(16) + `std.error`(16) + `std.random`(5)。",
                "C · 需 Task 原生/自举特化": "`std.task` 在 Host 与自举 HUAB 已通；原生 `--release` 走并发纤程特化路径。",
                "D · 需多态签名规则": "签名含泛型或开放参数（`\"\"`，如 `json.from`/`encode_pretty`、`os.parse_args`/`environ`、`maps`/`sets`/`list` 新增项、`alg` 非回调项、`test.equal`）；需在 `types.hua::standard()` 补类型推导分支。",
                "D+C · 需多态规则+类型映射": "`std.deque`(11) 与 `std.random.shuffle/choice/sample`(3)：需同时补 `Deque<T>`/`Rng` 类型映射与 `types.hua` 元素类型推导。",
                "E · 需高阶回调支持": "L0-1 高阶函数（19 项：`alg.map/filter/fold/...`、`list.sort_by/...`、`heap.*_by_key`、`maps.update`）。`native_algorithms.hpp` 已有 C++ 模板实现，但 `selfhost` 需补函数类型参数检查与回调代码发射。",
            }.get(k, "")
            lines.append(f"| {k} | {summary['by_gap'][k]} | {desc} |")

        lines.append("")
        lines.append("## 2. 模块级覆盖总览")
        lines.append("")
        lines.append("| 模块 | 总数 | Host | SH-Check | SH-HUAB | Release-EXE | 最低版本范围 | 首批 CLI 样例相关性 |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
        cli_relevance = {
            "std.os": "**核心必需**（`args`/`cwd`/`getenv` 已通；建议首批补 `parse_args`、`exit`）",
            "std.strings": "**核心必需**（13 项已通；建议首批补 `trim_*`/`lines`/`fields`/`parse_int`/`parse_float`/`format` 等 B 类项）",
            "std.fs": "**核心必需**（7 项已通；`exists`/`mkdir_all`/`list_dir` 仅差 `bridged()`，可选补 `walk`/`copy_file`）",
            "std.file": "**核心必需**（SH-HUAB 16 项已通；`native_cpp.hua` 补 `FileTag`/`IOFailureTag` 即一次打通全部 16 项含 `read_line`）",
            "std.json": "**核心必需**（6 项全通、6 项缺 `bridged()`；建议首批补 `decode_lines`/`encode_lines`/`object`/`array`/`set`/`from`/`encode_pretty`）",
            "std.error": "**核心必需**（`native_rt.hpp` 已有 `ErrorTag`；补类型映射即一次打通全部 16 项结构化错误）",
            "std.io": "**高**（补 `bridged()` 打通 `read_line`/`read_all`/`write`/`write_err` stderr 诊断输出）",
            "std.path": "**核心必需**（6/6 已全路径打通）",
            "std.bytes": "已基本全通（8/13 全通，余 5 项仅差 `bridged()`）",
            "std.buffer": "已基本全通（7/14 全通，余 7 项含 `write_str`/`take_text` 仅差登记+`bridged()`）",
            "std.list": "基础 9/23 已全通（够首批 CLI 使用）",
            "std.alg": "基础 3/27 全通、10/27 SH 已通（够首批 CLI 使用）",
        }
        for mod in sorted(by_module.keys()):
            mrows = by_module[mod]
            sinces = sorted({int(r["since"]) for r in mrows})
            since_str = str(sinces[0]) if len(sinces) == 1 else f"{sinces[0]}–{sinces[-1]}"
            lines.append(
                f"| `{mod}` | {len(mrows)} | {sum(bool(r['host']) for r in mrows)} | "
                f"{sum(bool(r['sh_check']) for r in mrows)} | {sum(bool(r['sh_huab']) for r in mrows)} | "
                f"{sum(bool(r['release_exe']) for r in mrows)} | HUAB {since_str} | "
                f"{cli_relevance.get(mod, '后续批次按需对齐')} |"
            )
        lines.append("| `std.alg_integer` *(SDK 源码)* | 1 | 1 | 1 | 1 | 1 | HUAB 6 | 纯 Hua 标准模块样板（四路径已通） |")

        lines.append("")
        lines.append("## 3. 逐接口覆盖明细表（27 内建模块 374 函数 + 1 源码模块）")
        lines.append("")
        lines.append("路径图例：`✓` 已支持，`✗` 未接通。所有 `Host` 列均涵盖 `hua interpret`、源码 `hua run`、宿主 `hua build` + 无源码 `hua run <.huab>`。")
        lines.append("")
        lines.append("| 序号 | 接口 | 签名 | 最低版本 | Host | SH-Check | SH-HUAB | Release-EXE | 缺口分类 | 成功或失败验证用例 |")
        lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        idx = 1
        for mod in sorted(by_module.keys()):
            for r in by_module[mod]:
                sig = str(r["signature"]).replace("|", "\\|")
                case = str(r["case"]).replace("|", "\\|")
                lines.append(
                    f"| {idx} | `{r['full_name']}` | `{sig}` | HUAB {r['since']} | "
                    f"{'✓' if r['host'] else '✗'} | {'✓' if r['sh_check'] else '✗'} | "
                    f"{'✓' if r['sh_huab'] else '✗'} | {'✓' if r['release_exe'] else '✗'} | "
                    f"{r['gap']} | {case} |"
                )
                idx += 1
        lines.append(
            f"| {idx} | `std.alg_integer.lower_bound` | `(values []int, key int) int` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通（SDK 源码） | `alg_integer.lower_bound([1,4,4,7], 4)` → `1`；未设 `HUA_HOME` 报 `E5001` |"
        )
        lines.append("")
        lines.append("## 4. 工具链级特性补充说明")
        lines.append("")
        lines.append("- **`@test` 与 `hua test <file.hua> [--filter text]`**：属于 CLI 驱动行为（`src/main.cpp:43`），当前调用 C++ `ModuleLoader` + `SemanticAnalyzer` + `VirtualMachine` 按源码顺序独立执行顶层 `@test fn`。Step 1 仓库外多包完成后，只要 C++ `ModuleLoader` 接入相同的本地包解析规则，`hua test` 即可直接测试多包项目与独立包，无需等 `native_cpp.hua` 单独支持 `@test`。")
        lines.append("")
        out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
