# 2026-11-27 第 5 步第一波验收记录：5A `std.csv` + 5B `std.regex` + 5C `std.process.start`（HUAB 32）

- **日期**：2026-11-27
- **基线**：`main@c72b40e` 之后（继第 4 步与 `tests/` 清理之后，尚未提交/推送 Git）
- **目标**：合并交付第 5 步第一波三项高频能力（共享一次 `HUAB format 32` 与一次 `huaBin/` 固定点重建）：
  1. **5A**：纯 Hua SDK 标准源码模块 `std.csv`（`std/csv.hua`，装配至 `huaBin/src/std/csv.hua`），基于 `std.stream.Reader`/`Writer` 实现 RFC 4180 流式 CSV 读写。
  2. **5B-regex**：无回溯、ReDoS 免疫的线性时间 Thompson NFA 正则引擎 `std.regex`（`include/hua/regex.hpp`、`src/regex_stdlib.cpp`），提供 6 个函数与不可变不透明句柄 `std.regex.Pattern`。
  3. **5C**：流式子进程管道 `std.process.start/stdin/stdout/stderr/wait/kill`（`src/process_stdlib.cpp`），导出活资源句柄 `std.process.Child`（避开语言关键字 `spawn`），Windows 端采用 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 与 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 严格隔离句柄并杜绝孤儿进程，禁止 `clone`/`spawn`（`E3008`/`E4104`）。

## 关键实现与修正

1. **宿主与自举双端接入**：
   - `include/hua/value.hpp` / `native_rt.hpp` / `stdlib.hpp`：新增 `RegexValue`、`ProcessChildValue` 及对应原生标签；`src/stdlib.cpp` 新增 12 个 `since 32` 标准函数，`src/archive.cpp` 升级为 `HUAB format 32`（`读取 1–32`）。
   - `src/module.cpp`、`scripts/package-windows-sdk.py`、`tests/tools/sync_stdlib_contract.py`：注册第 3 个纯 Hua SDK 源码模块 `std.csv`；运行 `sync_stdlib_contract.py --bootstrap --write` 更新 `selfhost/stdlib_manifest.json` 并生成 `selfhost/{catalog,signatures,archive,native_cpp}.hua`。
   - `selfhost/{modules,types,lowering,archive,bytecode_verify,native_cpp}.hua`：补齐 `std.regex.Pattern`、`std.process.Child` 与 `std.csv` 支持。
2. **预检阶段发现并修复的两处问题**：
   - `src/regex_stdlib.cpp`：`add_shorthand` 中的 `add_range` 误写入 `cc` 而非 `tmp`，修正后 `\D`/`\W`/`\S` 取反语义正确。
   - `std.process.spawn`：`spawn` 为 Hua 并发关键字，无法作为成员名解析，统一更名为 `std.process.start`。

## 自举不动点与验收结果

- **`huaBin/` 固定点重装配**（`build_step1_huabin.py`，临时 staging 目录验后已清理）：
  - `G1 == G2` PE SHA-256：`903837f6eceeb4cb6f6a9e2786adc30e9d3d6a7fbf499cb8ca141cb31c6e0fec`
  - C++ 生成不动点 SHA-256：`6e7e64b7061a18cc383a6a9a446a7674f84b5f5379f9f1ad6945c319abb1ce6a`
  - `A2` (`hua-archive.exe`) SHA-256：`5b6707867e739d5ece7f70abe5388d381f26ae50e881101d64a030eba29a0a31`
  - `C2 (19.392s) == C3 (160.005s, 756,482,587 步)` HUAB SHA-256：`c898b9e0ac1ee175593870010fe5bb305341c074aa0ec28df8b264f60a9e510d`（`5,801,715` 字节，`86,528` 条指令）
- **全 4 路径端到端验收**（`python tests/step5_csv_regex_process_tests.py --sdk huaBin`，耗时 `40.66s`）：
  - `5B std.regex`（含 `\d`/`\D`/`\w`/`\W`/`\s`、量词 `{2,4}`、`find`/`find_all`/`replace_all`/`split`、非法语法 `Err`、`(a+)+b` 对 30 个 `'a'` ReDoS 线性时间免疫）+ `5C std.process.start`（双向 stdin/stdout/stderr 管道、非零退出码、幂等 `kill`、`wait` 超时强杀）：全部 4 路径（`hua run`、宿主 HUAB 32、自举 HUAB 32、`hua build --release` EXE）全过。
  - `5C clone(Child)` 负例：精确报 `E3008`。
  - 集成应用 `5A std.csv + 5B std.regex -> packages/sqlite`（含引号逗号/内嵌换行/`""` 转义/中文 UTF-8/未闭合引号 `Err`、正则邮箱过滤入 SQLite、再经 `std.stream.Writer` 写出 CSV）：全部 4 路径（含复制到无 SDK `PATH` 干净目录独立运行的 `--release` EXE）全过。
- **CTest 回归**：`archives_extensions`（`21.90s`）、`module_contract`（`12.24s`）、`stdlib_contract`（`0.59s`）`3/3` 全过。
- **标准库水位**：31 内建模块 / 418 函数（自举 `343/418`、`--release` `334/418`、`22/31` 模块四路径全通）+ 3 个纯 Hua SDK 源码模块（`std.alg_integer`、`std.sse`、`std.csv`）。
