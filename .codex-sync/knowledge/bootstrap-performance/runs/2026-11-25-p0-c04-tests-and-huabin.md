# 2026-11-25 P0 收尾：C04 白盒单测适配、第二台实体机复现与 huaBin 重装配验收

## 1. 任务与环境身份

- **目标**：在第二台配置不同的 Windows x64 实体机（`E:/project/hua`，Python `3.14.3`）接手 `main@60dcb67`，完成 C04 算法改造后的两项 P0 收尾：(1) 适配 3 项直接抽取 `selfhost/capabilities.hua` 私有 `View.origins` 的白盒单测；(2) 用本机新建 C++20 宿主与 15 模块 `selfhost/`（含 `origin_pool.hua`）重建原生编译器与种子，更新官方 `huaBin/` 及便携 ZIP，并完成仓库外搬移验收。
- **代码基线**：`main` @ `60dcb67`（便携工具链 `.tools/llvm-mingw-20260922-ucrt-x86_64` Clang `23.1.2`、`.tools/cmake-4.4.4-windows-x86_64`、`.tools/ninja-1.13.2`、`.tools/wasmtime-v49.0.2-x86_64-windows-c-api`）。
- **未改动生产编译器/运行时源码**：`selfhost/` 与 `src/`、`include/` 保持零修改。

## 2. C04 白盒单测与模块数断言适配

- **改动文件**：
  - `tests/capability_fixture_support.py`：新增针对 `'import origin_pool as opool' in text` 的源码派生适配（注入 `CapExportView` / `CapExportValue` 及 `cap_decode_origins` / `cap_test_oid` / `cap_add_origin` / `cap_export_value` 测试桥接辅助，自动为 `fn main()` 注入 `opool.Pool` 参数与已内联薄包装，保留 `c04-symbolic` 和 `c04-r1-r2` 实验 `--source` 兼容）。
  - `tests/capability_instantiation_tests.py`、`tests/capability_metadata_tests.py`、`tests/capability_seed_tests.py`：允许跳过已内联的薄转发函数，按需复制同目录 `origin_pool.hua` 到隔离测试目录，并在分解式来源池模式下规约局部 `!` 原子的无符号集合标志。
  - `tests/windows_release_cache_tests.py`、`tests/tools/n3_5_release_acceptance.py`：同步 `native_main.hua` 新增 `origin_pool.hua` 后的分模块编译单元数断言（`13 -> 14`）。
- **验证结果**：CTest 定向运行 9 项 C04 测试（`capability_metadata`、`capability_instantiation`、`capability_dependencies`、`capability_prepared_supervisor`、`capability_symbolic`、`capability_transforms`、`capability_flow_state`、`capability_parameter_origins`、`capability_flow_metadata`）**`9/9` 全部通过**（总耗时 `94.18s`，其中 `capability_flow_metadata` `279` 项断言、`capability_parameter_origins` `17` 项契约断言均通过 AST / 源码 VM / 无源码 HUAB 三路径对照）。

## 3. 第二台实体机 15 模块自举构建、跨机复现与 huaBin 装配

原始证据与回执位于 `tests/artifacts/p0-huabin-20261125/summary.json`（旧 `huaBin` 备份于同目录 `previous-huaBin`）。

| 步骤 | 实测耗时 | 关键指标 / 产物身份 |
|---|---:|---|
| 本机 CMake Release 全量编译 `tests/build`（84 目标） | `~35s` | `hua.exe` SHA-256 `a95c9fbecc0d30ab...` |
| 过渡 `stage1-sdk` 装配（含旧 `G0`） | `71.995s` | 构建新 15 模块 `C1` 种子与可搬移 `release_runtime.pch` |
| `G0` 构建 14 单元新原生生成器 `G1`（`native_main.hua`） | `142.035s` | 生成 `30.157s` + 编译 14 对象 `111.078s` + 链接 `0.124s`；`17,451,008` 字节，SHA-256 `858be9fc1a67181f...` |
| `G1` 自举构建 `G2`（`kit-input/generator.exe`） | **`6.742s`** | 生成 **`6.412s`**（较旧生成器提速 `4.70×`），14 单元全部 `hit` + `link-hit`，PE SHA-256 与 `G1` 完全一致（`858be9fc...`） |
| `G1` 构建 15 模块原生归档编译器 `A2`（`kit-input/archive.exe`） | **`25.180s`** | 生成 `6.8s`，复用 11 个共享前端对象，仅编译 5 个后端/启动对象；`16,971,264` 字节，SHA-256 `140f610cabb75fd6...` |
| `G1` vs `G2` 对 `native_main.hua` `emit` C++ 不动点 | `5.982s` / `5.924s` | 两代 C++ 逐字节一致，SHA-256 `f100d2e5ee5d5cc8...` |
| 原生 `archive.exe` 编译 15 模块自身 `C2` | **`6.620s`** | `5,095,787` 字节，`hua check OK`，SHA-256 **`566b7a698e394c9f0f1b9475d74b921b97a2c8294460fafabe17dc6edf0781ec`** |
| 本机 VM 运行 `C2` 编译同一 15 模块自身 `C3` | **`61.855s`** | **`682,337,327` 步**，`peak_calls = 53`，`C2 == C3` 逐字节一致，**与原实体机 2026-11-24 记录 SHA-256 及步数 100% 跨机复现** |
| 正式 `huaBin/` 与便携 ZIP 装配 | `64.933s` | 清单共 `5,129` 个文件（`439,412,212` 字节，含 `src/compiler/origin_pool.hua`）；ZIP `127,323,757` 字节，SHA-256 `e33791a576b4d3fd...` |

## 4. 更新后 huaBin 四套仓库外 SDK 验收

1. **`tests/windows_sdk_stdlib_tests.py huaBin`**：仓库外搬移、双编译器构建 `std.alg_integer`、无源码运行、防同名遮蔽、Windows Junction 拒绝、坏清单诊断及修改纯 Hua 标准库实现重建全部通过。
2. **`tests/windows_release_sdk_tests.py --sdk huaBin --rebuild`**（中文与空格隔离目录，证据 `verify-release-sdk/report.json`）：
   - `5,129` 个清单文件尺寸与 SHA-256 逐一校验通过。
   - 独立应用冷构建与运行：`你好.hua`（标量 `477,184` 字节，`1.301s`）、多模块容器 `业务.exe`（`1,927,168` 字节，`1.212s`）、`std.alg_integer`（`1,913,856` 字节，`0.679s`），`llvm-readobj` 校验 PE 仅导入 Windows 系统与 UCRT DLL。
   - 在隔离副本中修改 `src/compiler/native_cpp.hua` 重建候选原生编译器 `hua-candidate.exe` 耗时 **`113.680s`**（其中 C++ 生成仅 `6.002s`），探针 `9+4` 输出由 `13` 变为 `5`。
3. **`tests/windows_native_run_tests.py --sdk huaBin`**（证据 `verify-native-run/native-run.json`）：
   - 中文空格路径与特殊转义参数在源码 VM 与 `hua run --release` 一致，临时目录零残留，与 `hua build --release` 共享缓存命中（`0/0/0` 调用）。
   - 7 轮中位数：`hello` 冷运行 `540.724 ms`、热运行 **`98.366 ms`**；300 万次整数循环冷运行 `556.418 ms`、热运行 **`106.033 ms`**。
4. **`tests/windows_release_cache_tests.py`**（证据 `verify-release-cache/report.json`）：整次命中、损坏恢复、传递内容修改、同 `mtime` 头文件改动、运行库切换、并发构建及菱形枚举布局失效共 29 项场景全部通过。
