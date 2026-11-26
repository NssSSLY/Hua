# 2026-11-26 应用路线第 3 步验收：Native ABI v2、随包 DLL 分发与 SQLite 扩展包

## 1. 任务与基线身份

- **任务**：按 3A → 3B → 3C 顺序完成应用驱动五步路线第 3 步（同步 Native ABI v2、`NativeHandle` 代次资源生命周期、多包 `native_libs` 随包 DLL 分发、`HUAB format 30 / native_abi 2`、自举/原生发布双端打通及首个原生扩展包 `packages/sqlite/` 仓库外四路径验收），并核查补全 `docs/语言百科与设计核对.md`（`HUA-14.13`）中的全部现有错误码。
- **代码基线**：`main@94e2442` 之上叠加第 2 步与本轮第 3 步改动（未提交/未推送 Git）。
- **平台与工具链**：Windows x64，`llvm-mingw-20260922-ucrt-x86_64`（Clang 23.1.2 / lld），Python 3.12 附带 `sqlite3.dll`（SQLite 3.42.0）。

## 2. 主要实现改动

### 3A · 宿主 Native ABI v2 与资源控制块
- `include/hua/native.h`：保持 `HUA_NATIVE_ABI 1` 与 `hua_module_entry_v1` 不变；新增 `HUA_NATIVE_ABI_V2 2`、`hua_api_v2`、`hua_export_v2`、`hua_module_v2`、`hua_module_entry_v2`、`HUA_BYTES`/`HUA_HANDLE` 及 `HUA_HANDLE_OK/CLOSED/POISONED/INVALID`。
- `include/hua/value.hpp` / `src/value.cpp` / `src/sema.cpp` / `src/task_engines.cpp` / `src/module.cpp`：新增 16 字节 `NativeHandleValue`（`Value::Data` 保持 48 字节），拒绝 `clone(NativeHandle)`（`E3008`）与 `spawn`/Task 快照捕获（`E3008`/`E4104`）；支持包内 `.huam`（`HUA_NATIVE 1|2`）与 `hua.package.json` `native_libs`（校验相对路径与防逃逸，违例报 `E5001`/`E5004`）。
- `include/hua/native_external.hpp` / `src/native_external.cpp` / `src/external.cpp`：拆分零 `wasmtime` 依赖的 `NativeExternalHost`（含互斥锁保护的代次 `HandleTable` 与 `FreeLibrary` 前 LIFO 析构器清理）；宿主 `ExternalRegistry` 同步支持 ABI v1/v2/WASM。
- `src/archive.cpp`：新增 `HUAB format 30`（`native_abi 2`），校验外部模块 `abi_version` 与头部一致性，非法拒绝 `E6002`。

### 3B · 自举编译器、原生运行时与 Windows 便携目录发布
- `include/hua/native_rt.hpp` / `src/native_rt.cpp`：接入 `NativeHandleTag`、`register_external_module`/`init_external_module`/`call_external` 与退出 `ExternalCleanupGuard`，链接 `NativeExternalHost` 而不引入 `wasmtime.dll`。
- `selfhost/{catalog,modules,types,lowering,bytecode,bytecode_verify,archive,native_cpp,native_main,release_cache,release_main}.hua`：同步支持 `HUA_NATIVE 1|2`、`NativeHandle`、`ExternalInit`（`role="external"` 与 `shape` 校验）、`HUAB format 30 / native_abi 2`、`native_libs` 伴随 DLL 解析与复制到 `<out_dir>/native/<pkg>/`、以及 N3 缓存二进制哈希与缓存命中时自动恢复伴随 DLL。

### 3C · 首个原生扩展包 `packages/sqlite/` 与百科错误码补全
- `packages/sqlite/{sqlite_native.c,sqlite_native.huam,package.hua,hua.package.json}`：窄 C ABI v2 包装（调用 `sqlite3_initialize()` 以兼容 `SQLITE_OMIT_AUTOINIT` 的 `sqlite3.dll`，按 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` 加载同目录 `sqlite3.dll`；将 SQLite 错误映射为 `std.error.Value`，含 `domain="sqlite"`、`code` 与 `native_domain="sqlite3"`/`native_code`），Hua 层提供 `Db`/`Stmt`/`Param`、参数绑定与 `begin`/`commit`/`rollback` 事务接口。
- `docs/语言百科与设计核对.md`（`HUA-14.13`）：全库扫描 `src/`、`include/`、`selfhost/` 全部 61 个独立 `E\d{4}` 诊断码，补齐缺失的 9 项（`E0002`–`E0004`、`E4900`、`E8201`–`E8204`、`E8302`）并更新 ABI v2 相关条目释义，漏缺数为 `0`。

## 3. 代际不动点与 `huaBin/` 重装配实测

命令：`python tests/tools/build_step1_huabin.py --work-dir tests/artifacts/step3-huabin-20261126`

- `G1 == G2` PE SHA-256：`e17f22f9f0dde8e622b38982c948e3bcab7ab2e41825f0af800352e669733852`（`build_g1_s = 276.821s`，`build_g2_s = 15.423s`）
- `G1 == G2` 生成 C++ SHA-256：`c56f1186653e427ecb06e0116901b7e840f42d803200e86f9909dc9ca5daca8b`
- `A2` 原生归档编译器：`58.242s`（11 个前端模块全命中缓存，SHA-256 `63b3f06af9d1225e...`）
- `C2`（`15.796s`）`==` `C3`（`124.828s`，`743,192,893` 步，`peak_calls = 53`）HUAB SHA-256：`c23f98c84514686f96bcdc97680c4226b0e47443b5586e4de5f004048fe4c825`（`5,708,323` 字节，`85,040` 指令，`format 17`）
- 官方 `huaBin/` 同步完成。

## 4. 验收与回归结果

| 测试套件 | 结果 | 关键覆盖 |
|---|---|---|
| `python tests/sqlite_package_tests.py --sdk huaBin` | **PASS** | Path 1（`hua run`）、Path 2（Host HUAB 30 / `native_abi 2`）、Path 3（Selfhost HUAB 30 / `native_abi 2`）、Path 4（`hua build --release` 复制到无 `PATH`/`HUA_HOME` 干净目录运行）、Path 4b（N3 缓存命中恢复 EXE + `sqlite_native.dll` + `sqlite3.dll`）、缺 DLL `E7001`；验证中文文本、含 `\0` 与非 UTF-8 `BLOB` 往返、UNIQUE 约束回滚、幂等关闭与关闭后 `code == "closed"` |
| `python tests/native_abi_v2_tests.py --hua huaBin/bin/hua.exe` | **PASS** | ABI v2 `Bytes`/结构化错误/句柄生命周期/LIFO 析构 + 6 项负向诊断（`E7001`/`E7002`/`E7004`/`E3008`/`E4104`） |
| `ctest --test-dir tests/build -R "stdlib_contract|archives_extensions|module_contract|error_foundation|concurrency"` | **PASS** | `stdlib_contract`（`0.47s`）及宿主归档/模块/错误/并发合同全过 |
| 百科错误码扫描（`src/` + `include/` + `selfhost/`） | **PASS** | 61/61 独立 `E\d{4}` 诊断码已全部收录于 `docs/语言百科与设计核对.md`（缺失 `0`） |

## 5. 过程中定位并解决的关键问题

1. **零 Wasmtime 链接边界**：初版 `native_rt.cpp` 引用 `external.cpp` 导致 `--release` 产物链接 `wasmtime_*` 未定义符号；通过将纯原生主机拆至 `native_external.hpp/cpp` 彻底隔离。
2. **CPython `sqlite3.dll` `SQLITE_OMIT_AUTOINIT` 崩溃**：直接调用 `sqlite3_open_v2` 触发 `0xc0000005`；在 `load_sqlite_dll()` 中先调用幂等 `sqlite3_initialize()` 后彻底解决。
3. **自举 C05 `ExternalInit` 与 `role="external"` 校验**：`selfhost/lowering.hua` 补齐 `shape("ExternalInit", 0)` 与 `verify()` 对 `role="external"` 的白名单后，自举 HUAB 30 与 `--release` 双端完全贯通。

## 6. 结论与下一步

第 3 步全部既定目标与验收点通过；下一步按五步路线进入**第 4 步：并行推进流式 I/O（`Reader/Writer`）与 HTTPS 客户端**。
