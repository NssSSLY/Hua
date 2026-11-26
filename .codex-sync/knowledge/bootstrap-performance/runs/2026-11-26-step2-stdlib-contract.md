# 应用驱动路线第 2 步验收记录：单一权威标准库合同与 103 项纯表项双端打通

日期：2026-11-26  
基线：`main@94e2442`  
设计方案：[2026-11-26-step2-stdlib-contract-plan.md](../designs/2026-11-26-step2-stdlib-contract-plan.md)  
原始构建摘要：`tests/artifacts/step2-huabin-20261126/summary.json`

## 1. 实施改动

1. **单一权威清单与双向校验/生成工具**：
   - 新增 `selfhost/stdlib_manifest.json`（`hua.stdlib.manifest.v1`），收录 38 项全局核心内建、1 项 SDK 纯 Hua 源码模块（`std.alg_integer`）以及 27 个内建模块全部 374 个函数的宿主签名、自举签名、多态规则、`since` 版本与四端状态。
   - 新增 `tests/tools/sync_stdlib_contract.py`：支持 `--bootstrap-manifest`、`--write`（确定性生成按模块两级分桶的 `selfhost/catalog.hua`、`selfhost/signatures.hua`、`selfhost/archive.hua::load_format`、`selfhost/native_cpp.hua::bridged`）、`--check`（零字段差异核对 `src/stdlib.cpp` + 四份生成文件逐字节比对 + `huaBin/src/compiler/` 一致性核对）与 `--self-test`（4 项篡改负向防漏自检）。
   - 在 `CMakeLists.txt` 注册 `stdlib_contract` CTest（`--check --self-test`），在 `scripts/package-windows-sdk.py` 中将 `selfhost/stdlib_manifest.json` 随 SDK 装配至 `huaBin/src/compiler/stdlib_manifest.json`。
2. **8 模块 103 项双端打通与自举类型规则对齐**：
   - `selfhost/types.hua`：将 `std.math` 参数类型检查由写死 `!= "float"` 对齐 `src/sema.cpp:638` 改为 `s.parameters[i] != "" && arguments[i].type_name != s.parameters[i]`；为 `std.test.equal`/`not_equal` 补齐禁 `void` 与双向兼容检查（对齐 `src/sema.cpp:651-652`）。
   - 一次性打通 `std.math`(53/53)、`std.encoding`(17/17)、`std.time`(14/14)、`std.crypto`(5/5)、`std.log`(6/6)、`std.test`(4/4)、`std.gc`(4/4)、`std.process`(2/2)。
   - 更新 `tests/tools/audit_stdlib_coverage.py` 独立解析分桶化 `signatures.hua`、`archive.hua`、`native_cpp.hua` 与 `types.hua`，并刷新 [逐接口覆盖表](../designs/2026-11-25-stdlib-coverage-matrix.md)。
3. **8 模块四路径差分测试套件**：
   - 新增 `tests/stdlib_batch2a_parity_tests.py`，覆盖 12 项 `HUAB 6–28` 版本门禁对照、4 项 `E3004` 三端静态拒绝（无残留产物）、8 模块全量四路径运行对照（含 `std.process.run/run_with`、`std.log` logfmt、失败 `test.equal(1, 2)` 的 `E4900` 诊断对照）。

## 2. 7 项验收标准实测结果

| 编号 | 验收项 | 实测结果 |
|---|---|---|
| 1 | 覆盖率跃升（`audit_stdlib_coverage.py`） | **通过**：`Host 374/374`、`SH-Check 297/374`、`SH-HUAB 297/374`、`Release-EXE 288/374`；四路径全通内建模块 `10/27 → 18/27`（外加 `std.alg_integer`）；B 类纯表项缺口 `101 → 0`。 |
| 2 | 单一权威防漏门禁（`ctest -R stdlib_contract`） | **通过**：`Test #109: stdlib_contract Passed (0.47 sec)`，含 `src/stdlib.cpp`、`selfhost/`、`huaBin/` 全量核对与 4/4 篡改负向自检。 |
| 3 | HUAB 版本门禁（`format 6–28`） | **通过**：`HUAB format gates (12/12)`，`math(6/12/21/22)`、`crypto(13)`、`time(15)`、`log/test(18)`、`encoding(12/23/24)`、`process(28)` 宿主与自举 `.huab` 头部 `format` 逐项一致并通过 `hua check`。 |
| 4 | 8 模块四路径运行一致（`stdlib_batch2a_parity_tests.py`） | **通过**：仓库外独立目录 `hua run`、宿主无源码 `.huab`、自举无源码 `.huab`、独立 `.exe` 四路输出、logfmt `stderr`、`E4900` 诊断与退出码完全一致（`std.gc` 按非负计数契约验证，`std.process` 实跑 `hua.exe version` 通过）。 |
| 5 | 静态类型拒绝一致（`E3004`） | **通过**：`Static E3004 rejections across 3 frontends (4/4)`（`math.sin(1)`、`math.gcd(1.5, 2.0)`、`math.popcount(u8(1))`、`test.equal(1, "1")` 均被 `hua check`、`hua-archive.exe`、`hua build --release` 以 `E3004` 拒绝且无残留产物）。 |
| 6 | 代际不动点与性能边界 | **不动点通过，步数如实记录**：`G1 == G2` PE SHA-256 `fd81223a24fbcfe8a5ad9a13be71810d0433a762daacd8db50841d2854263fcc`，发射 C++ SHA-256 `31c3686a179ada179f791682e96af171e64ab12290ed61757a478a8e19f58fc9`；`A2` `58.889s`（11 个前端对象全命中缓存）；`C2(15.733s) == C3(125.772s)` HUAB SHA-256 `725d5e32f263d897f3f112f2ae8bdb31b78f19f150d9ee8d66d5bdbaa6191131`（`5,608,748` 字节、`83,666` 条指令、`format 17`、`CRC32 1761755607`）。`C3` VM 步数为 `730,265,274` 步（`peak_calls=53`），较第 1 步基线 `728,213,138` 步增加 `+2,052,136` 步（`+0.28%`），系自举编译器源码新增 91 项标准库表项（产物 `+161 KB`）被编译所致，分桶查找将整体步数变化控制在 `0.3%` 以内。 |
| 7 | 多包与 4 套 SDK 回归 | **通过**：`local_package_cli_tests.py`（`36/36` 负例诊断对照）、`windows_sdk_stdlib_tests.py`（非沙箱，绕过 `mklink /J` 限制）、`windows_release_sdk_tests.py`、`windows_native_run_tests.py`、`windows_release_cache_tests.py`（28 项缓存场景）全部通过。 |
