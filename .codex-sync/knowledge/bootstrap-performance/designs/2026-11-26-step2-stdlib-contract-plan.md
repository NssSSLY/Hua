# 第 2 步详细设计方案与验收标准：单一权威标准库合同与首批纯表项双端打通

日期：2026-11-26  
状态：**已实施并验收**（详见 [第 2 步验收记录](../runs/2026-11-26-step2-stdlib-contract.md)）。

---

## 1. 背景与现状症结

截至第 1 步验收完成（[交付记录](../runs/2026-11-26-step1-multi-package-cli.md)），374 个宿主内建标准函数的四路径覆盖如下（详见 [逐接口覆盖表](2026-11-25-stdlib-coverage-matrix.md)）：

- `Host`（解释器 / 源码 VM / 宿主 HUAB）：`374 / 374`（外加 SDK 纯 Hua 源码模块 `std.alg_integer`）
- `SH-Check` / `SH-HUAB`（15 模块自举前端与 `hua-archive.exe`）：`206 / 374`（`10 / 27` 个内建模块全通）
- `Release-EXE`（`hua build --release`）：`185 / 374`

当前标准库元数据手工分散维护在 6 处：
1. `src/stdlib.cpp::standard_functions()`（宿主权威表）；
2. `selfhost/catalog.hua`（模块与导出名/类型名）；
3. `selfhost/signatures.hua`（206 条扁平顺序 `if target == "..."` 线性比较）；
4. `selfhost/archive.hua`（`type_format` / `load_format` 的 `HUAB 6–29` 门禁）；
5. `selfhost/native_cpp.hua`（`bridged()` 单一大数组线性扫描）；
6. `docs/标准库与内建接口.md`（公开接口文档）。

继续手工扩面存在两类隐患：**跨端漏接与 `since` 版本漂移**，以及**表项膨胀导致自举编译期线性查找开销放大**。

---

## 2. 详细实现方案

### 2.1 单一权威清单：`selfhost/stdlib_manifest.json`

在正式目录 `selfhost/` 下维护机器可读清单 `selfhost/stdlib_manifest.json`（Schema `"hua.stdlib.manifest.v1"`），并随 `scripts/package-windows-sdk.py` 与 `selfhost/contract.json` 一并装配到 `huaBin/src/compiler/`：

- **`core_builtins`（38 项）**：登记全部公开全局内建名及 `native_cpp.hua` 实现方式（`typed` / `bridged` / `pending`）。
- **`source_modules`（当前 1 项 `std.alg_integer`）**：登记纯 Hua 源码标准模块名、SDK 相对路径与最低 HUAB 版本，与 `huaBin/manifest.json` 保持一致。
- **`modules`（27 个内建模块、374 个函数）**：每项固定记录：
  - `module`、`name`、`exported_types`；
  - `host_parameters`、`host_result`（与 C++ `StandardFunction` 逐项一致）；
  - `selfhost_parameters`、`selfhost_result`、`polymorphic_rule`（`"none"` | `"math_exact"` | `"test_equal"` | `"json_any"` | `"alg"` | `"list"` | `"task"` 等）；
  - `mutates_first`、`writable_result`、`callback`、`since`（`3..29`，归档门禁自动计算为 `max(6, since)`）；
  - `backends`：`{"host": true, "selfhost_check": bool, "selfhost_huab": bool, "native_release": "none" | "bridged" | "inline"}`。

### 2.2 生成与双向强校验工具：`tests/tools/sync_stdlib_contract.py` + `stdlib_contract` CTest

1. **`--write`（确定性代码生成 + 分桶查找优化）**
   从 `selfhost/stdlib_manifest.json` 确定性生成四份自举表区：
   - `selfhost/catalog.hua`：生成 `module()`、`exports()`、`type_export()`；
   - `selfhost/signatures.hua`：改为**按模块前缀两级分桶查找**（先匹配模块，再匹配函数名），将最坏比较次数从约 300 次降至约 25 次以内；
   - `selfhost/archive.hua`：按模块与 `since > 6` 自动生成 `load_format()`；
   - `selfhost/native_cpp.hua`：将 `bridged()` 改为按 `$std$<module>$` 前缀分桶匹配。

2. **`--check`（注册为 CMake `stdlib_contract` 测试，耗时 `< 2s`）**
   - 核对 `src/stdlib.cpp::standard_functions()` 全部 374 项与 `selfhost/stdlib_manifest.json` 零字段差异；
   - 核对清单内部自洽（`native_release != "none"` 必含 `selfhost_check == true` 且类型受 `cpp_type` 支持；`selfhost_huab == selfhost_check`）；
   - 内存重新渲染四份自举表并与磁盘文件逐字节比对，发现手工漂移立即失败；
   - 若存在 `huaBin/`，核对 `huaBin/src/compiler/` 与 `selfhost/` 及 `huaBin/manifest.json` 一致。

### 2.3 首批纯表项 101 项 + `std.test` 闭合（共 103 项）双端打通细节

| 模块 | 打通数 / 总数 | `HUAB since` | 实现细节与边界说明 |
|---|---:|---|---|
| `std.math` | `53 / 53` | `6, 12, 21, 22` | **必改点 1**：`selfhost/types.hua:336` 原写死 `arguments[i].type_name != "float"`，须对齐 `src/sema.cpp:638` 改为 `s.parameters[i] != "" && arguments[i].type_name != s.parameters[i]`，同时放行 26 个整数函数（`gcd`/`lcm`/`isqrt`、5 个位运算、18 个 `checked/wrapping/saturating`）与 3 个无参常量（`pi`/`e`/`tau`），并继续以 `E3004` 拦截 `math.sin(1)` 隐式整型转浮点及 `i32`/`u8` 混用。 |
| `std.encoding` | `17 / 17` | `12, 23, 24` | hex/base64/base64url、UTF-8 校验编解码及大小端 `pack_*`/`unpack_*`。`utf8_decode` 返回 `multi<int,int>`，复用 `native_cpp.hua` 的 `std::tuple` 与 `native_rt.hpp` 的 `From<std::tuple<...>>` 自动解包。 |
| `std.time` | `14 / 14` | `6, 15` | 补齐 12 项时长解析格式化、RFC 3339 与 `utc_parts`（7 元组 `multi<int,int,int,int,int,int,int>`）。 |
| `std.crypto` | `5 / 5` | `13` | `random_bytes`/`uuid4`/`sha256`/`hmac_sha256`/`equal`；Windows 静态运行库已链接 `bcrypt`。 |
| `std.log` | `6 / 6` | `18` | `debug`/`info`/`warn`/`error`/`set_level` 及 `with_fields(string, string, map<string,string>)`（`to_value(Map<Str,Str>)` 已支持装箱）。 |
| `std.test` | `4 / 4` | `18` | `assert`/`fail` 为纯表项；**必改点 2**：在 `selfhost/types.hua::standard()` 为 `equal`/`not_equal` 补 4 行对齐 `src/sema.cpp:651-652`（禁 `void`，两参数须双向兼容，否则报 `E3004`），使 `std.test` 完整 4/4 闭合（`@test` 属性另属语法/CLI 特性）。 |
| `std.gc` | `4 / 4` | `6` | `collect`/`live`/`allocated`/`collected`。**语义边界**：双端均可安全调用并触发文件检查点，但因原生后端标量/局部值不经 VM 装箱堆，不断言 VM 与原生 `allocated()` 绝对计数相等。 |
| `std.process` | `2 / 2` | `28` | `run`/`run_with`（返回 `multi<int,Bytes,Bytes,string>`）。真机 Windows 验收子进程执行，沙箱内做编译/归档校验与受控验证。 |

---

## 3. 实施步骤

1. 编写 `tests/tools/sync_stdlib_contract.py` 并生成权威清单 `selfhost/stdlib_manifest.json`，开启上述 8 模块 103 项双端开关。
2. 在 `selfhost/types.hua` 补齐 `std.math` 精确类型检查与 `std.test.equal/not_equal` 同类型检查；执行 `--write` 生成分桶化的四份自举表文件；更新 `scripts/package-windows-sdk.py` 随包打包 `stdlib_manifest.json`。
3. 在 `CMakeLists.txt` 注册 `stdlib_contract` CTest，包含篡改清单/表项必败的负向防漏自检。
4. 运行 `tests/tools/build_step1_huabin.py` 完成 `G1 == G2`、`A2`、`C2 == C3` 不动点校验与 `huaBin/` 重装配。
5. 新增 8 模块四路径差分验收套件 `tests/stdlib_batch2a_parity_tests.py`，运行全套 SDK/缓存回归，刷新覆盖表并同步文档。

---

## 4. 验收标准（7 项硬指标）

1. **双端覆盖率跃升（`audit_stdlib_coverage.py` 实测）**：
   - `SH-Check` / `SH-HUAB`：`206/374 → 297/374`；
   - `Release-EXE`：`185/374 → 288/374`；
   - 四路径 100% 全通的内建模块：`10/27 → 18/27`（外加 `std.alg_integer`）。
2. **单一权威防漏门禁生效（`ctest -R stdlib_contract`）**：`--check` 通过，且负向用例证明漏接任一后端、改坏签名或漏填 `since` 时立即非零报错并定位到具体函数名。
3. **HUAB 版本门禁精确对齐（`format 6–28`）**：自举归档编译器编译调用 `math(12/21/22)`、`crypto(13)`、`time(15)`、`log/test(18)`、`encoding(12/23/24)`、`process(28)` 的程序，生成 `.huab` 头部 `format` 与宿主 `hua build` 完全一致并通过 `hua check`。
4. **8 模块四路径运行结果、错误码与 `stderr` 一致（`tests/stdlib_batch2a_parity_tests.py`）**：仓库外独立目录验证 `hua run`、宿主 `.huab`、自举 `.huab`、脱离源码 `.exe` 四路输出与退出码一致（`std.gc` 按非负性与环回收契约验收）。
5. **静态类型拒绝一致**：`math.sin(1)`、`math.gcd(1.5, 2.0)`、`math.popcount(u8(1))`、`test.equal(1, "1")` 在 `hua check`、`hua-archive.exe`、`hua build --release` 三端均以 `E3004` 拒绝且无残留产物。
6. **代际不动点与性能不退化**：15 模块自举达成 `G1 == G2` C++ 与 `C2 == C3` HUAB 逐字节不动点，且分桶化 `signatures.hua` 后 `C3` VM 总步数不高于第 1 步基线（`728,213,138` 步）。
7. **多包与 N3 缓存回归全绿**：`local_package_cli_tests.py` 与四套仓库外 SDK 回归（含 28 项缓存场景）全部通过。
