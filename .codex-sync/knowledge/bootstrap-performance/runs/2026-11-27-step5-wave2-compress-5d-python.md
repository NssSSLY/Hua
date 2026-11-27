# 2026-11-27 第 5 步第二波验收记录：5B-gzip `std.compress` + 5D 非回调标准库 + 5E `packages/python/`（HUAB 33）

- **日期**：2026-11-27
- **基线**：`main@c72b40e` 之后（继第 4 步、`tests/` 清理与第 5 步第一波之后，尚未提交/推送 Git）
- **目标**：完成第 5 步剩余主体切片并共享一次 `HUAB format 33` 与一次 `huaBin/` 固定点重装：
  1. **5B-gzip (`std.compress`)**：零三方 DLL 的纯 C++20 RFC 1951（stored + fixed/dynamic Huffman 解压、fixed Huffman + LZ77 哈希链压缩）与 RFC 1952 GZIP 实现（`include/hua/compress.hpp`、`src/compress_stdlib.cpp`），导出 `gzip_compress`/`gzip_decompress`/`gzip_reader`/`gzip_writer`（`since 33`），含 `max_bytes` 炸弹防护与 CRC32/ISIZE 校验。
  2. **5D 非回调多态/句柄标准库双端清零**：在 `include/hua/native_rt.hpp` 新增 `RngTag`、`OpaqueDeque<T>` 及 `sync_from_value` 原生 `List<T>`/`Map<K,V>` 就地修改回写；在 `selfhost/{types,type_rules,lowering,bytecode_verify,native_cpp}.hua` 与 `sync_stdlib_contract.py` 补齐 `random`(8)/`deque`(11)/`heap`(6)/`maps`(6)/`sets`(7)/`list`(20)/`alg`(14) 全部非回调规则（自举覆盖升至 `401/422`）。
  3. **5E Python Bridge SDK 封装**：新增 `packages/python/{hua.package.json,package.hua,python_native.huam}`（`hua.package.v1` 类型化封装，兼容 VM、HUAB 与 `--release`；`python_native.dll` 为可选构建产物）。

## 自举不动点与实测验收

- **`huaBin/` 固定点重装配**（`build_step1_huabin.py`，临时 staging 目录验后已清理）：
  - `G1 == G2` PE SHA-256：`f616fe8ed07b03c7058d37a514fb5bd8e63b917b51760e3d975674f2c2bffce0`
  - C++ 生成不动点 SHA-256：`2aafbcf02893e39d911e83824d19a99825783c1a38e9416ae4688ffeac2f8459`
  - `A2` (`hua-archive.exe`) SHA-256：`c123e787cb9b5b051809d645d2b4f1c97059214adccbd0c56ab459c2323a4c8d`
  - `C2 (19.870s) == C3 (175.473s, 784,461,934 步)` HUAB SHA-256：`36c7d6b3a60bbfe59e43c4a806fcde206743e689fe82158c3ff55b114635c80a`（`5,985,175` 字节，`89,555` 条指令）
- **四路径 E2E 验收**：
  - `python tests/step5_wave2_compress_5d_python_tests.py --sdk huaBin`（`15.09s` 全过）：`std.compress`（含空串、200 行重复日志 `<25%` 压缩率、与 Python `gzip` dynamic-Huffman 双向互操作、`limit`/`corrupt` 结构化错误、流式 `gzip_writer -> gzip_reader` 装饰器）+ `random`/`deque`/`heap`/`maps`/`sets`/`list`/`alg` 全部 4 路径通过（`HUAB format 33`）；`packages/python` 因本机 `tests/build` 未启用 CMake `hua_python_bridge` 跳过实跑。
  - `python tests/step5_csv_regex_process_tests.py --sdk huaBin`（`41.19s` 全过）：Wave 1 零回归。
- **CTest 回归**：`archives_extensions`（`23.63s`）、`module_contract`（`11.69s`）、`stdlib_contract`（`0.73s`）`3/3` 全过。
- **已知边界**：`std.simd` 4 函数因 `simd` 关键字仍在 `selfhost/parser.hua:126` 排除名单中，仅宿主 Path 1/2 可用；剩余 19 项高阶回调与 9 项 `--release` `std.task`（`async/spawn`）留待下一专项处理。
