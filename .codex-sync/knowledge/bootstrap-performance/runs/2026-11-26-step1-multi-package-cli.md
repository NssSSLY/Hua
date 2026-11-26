# 第 1 步交付记录：仓库外本地多包 CLI 与标准库逐接口覆盖基线（2026-11-25～26）

## 1. 任务目标与基线身份

- **基线提交**：`main@d5cd1e2`（Windows x64，`llvm-mingw-20260922-ucrt-x86_64` Clang 23.1.2，CMake 3.22.1 + Ninja Release）。
- **任务范围**：执行应用驱动五步路线**第 1 步**：
  1. 产出 374 个宿主内建标准函数 + `std.alg_integer` 的双端逐接口覆盖表（`tests/tools/audit_stdlib_coverage.py`）。
  2. 在宿主（`ModuleLoader`）与自举编译器（`selfhost/modules.hua` + N3 缓存输入追踪）双端同步实现 `hua.package.json`（`hua.package.v1`）本地路径依赖、包身份、`@pkg/<name>/...` 统一逻辑源码标识、`std` 保留名保护、同名/菱形目录冲突与循环导入诊断。
  3. 打通真实 CLI 所需的首批标准库双端签名、HUAB `since` 版本门禁（`6–29`）与 `--release` 原生桥接（`os`、`io`、`fs`、`file`、`buffer`、`strings`、`json`、`error`），并放开自举语法对二元 `Result<T, E>` 非 `string` 错误类型的限制。
  4. 重装配官方 `huaBin/`，在仓库外独立临时目录完成三包 CLI（`audit_cli` + `logschema` + `reportfmt`）在 `hua run`、宿主/自举 `.huab`、`hua run --release`、迁离源码后的独立 `.exe`、N3 缓存失效与 36 项负例诊断对照上的完整验收。

## 2. 核心实现与修复点

1. **双端本地路径包解析器（`include/hua/module.hpp`、`src/module.cpp`、`selfhost/modules.hua`、`selfhost/release_main.hua`）**：
   - 无 `hua.package.json` 时保持原有单目录相对解析不变；存在 `hua.package.json` 时启用 `hua.package.v1` 校验（唯一解码 JSON 键，拦截 `\u0070` 与 `p` 等转义重复键，`std` 保留名拦截，最多 128 个外部包，拒绝符号链接/目录联接）。
   - 统一逻辑 `file_id`：根包模块为入口根相对 `/` 路径（`main.hua`、`config.hua`），外部包模块为 `@pkg/<pkg>/package.hua`、`@pkg/<pkg>/<sub>.hua`；指向同一规范化目录的菱形依赖共享唯一 `Unit` 并仅初始化一次，指向不同目录或与本地文件/目录重名报 `E5004`，包级清单循环与模块级导入循环报 `E5002`。
   - `selfhost/modules.hua` 仅使用旧种子 `G0` 已支持的原语实现清单解析，保证 `G0 -> G1 -> G2` 无自举死锁；同时将全部 `hua.package.json`、目录探测与外部包 `.hua` 记入 `g.build_inputs` 以驱动 N3 发布缓存精确失效。
   - `selfhost/release_main.hua` 将前端错误格式统一渲染为 `error[E5xxx]: ...\n --> <file_id>:<line>:<col>`，`E0004` 退出码对齐为 `2`。
2. **首批 CLI 标准库双端打通与两处自举边界修复**：
   - 同步更新 `selfhost/{catalog,signatures,types,lowering,archive,native_cpp}.hua`：补齐 `std.os`（10/10）、`std.io`（4/4）、`std.fs`（19/19）、`std.buffer`（14/14）、`std.strings`（44/44）、`std.json`（23/23）、`std.error`（16/16）以及 `std.file` 原生不透明句柄映射（16/16），`clone` 接入静态 `E3008` 与原生 `hn::clone_checked` 文件句柄保护。
   - **修复 1（`selfhost/parser.hua`）**：移除 `compound_type` 中要求二元 `Result<T, E>` 的 `E` 必须为字面 `"string"` 的早期临时限制，开放 `Result<T, error.Value>`、`Result<T, file.IOFailure>` 与自定义错误结构体（`void` 错误类型仍由 `types.hua` 以 `E3004` 拒绝）。
   - **修复 2（`selfhost/native_cpp.hua` + `include/hua/native_rt.hpp`）**：`slice_type(t)` 原为 `strings.starts_with(t, "[]")`，会将未定型空数组字面量类型 `"[]"`（如 `unwrap_or(opts["config"], [])`）误判为带元素类型切片并切出空元素名报错；改为 `len(t) > 2 && strings.starts_with(t, "[]")` 并补充 `to_value(EmptySlice)` 与 `Array` 上下文标记。

## 3. 标准库双端覆盖实核变化

由 `tests/tools/audit_stdlib_coverage.py` 对宿主注册表、自举前端/归档与原生后端逐项核对（详见 [逐接口覆盖表](../designs/2026-11-25-stdlib-coverage-matrix.md)）：

| 维度 | 改造前（`d5cd1e2`） | 第 1 步完成后 |
|---|---:|---:|
| 宿主 VM / 解释器支持 | `374 / 374` | `374 / 374` |
| 自举前端检查（`types` + `capabilities`） | `134 / 374` | `206 / 374` |
| 自举字节码归档（`hua-archive` HUAB `6–29`） | `134 / 374` | `206 / 374` |
| 原生独立发布（`hua build --release`） | `61 / 374` | `185 / 374` |

## 4. 自举不动点、`huaBin` 重装配与验收结果

1. **代际不动点与 `huaBin` 重装配（`tests/tools/build_step1_huabin.py`）**：
   - `G0 -> G1`（`247.069s`）→ `G1 -> G2`（`153.820s`）：`G1` 与 `G2` 对 15 模块 `selfhost/native_main.hua` 生成的 C++ SHA-256 逐字节一致（`d62e0d324deea1295dd4f28f39827c76e0771397e8626802b8a6b2f6278006a1`）。
   - `G1 -> A2`（`52.350s`，复用 11 个前端缓存对象）：原生 `A2` 编译 15 模块 `selfhost/archive_main.hua` 得 `C2`（`14.756s`），宿主 VM 运行 `C2` 编译同源得 `C3`（`111.641s`，`728,213,138` 步，`peak_calls=53`）；`C2 == C3` 逐字节一致（`5,447,071` 字节，`format 17`，`80,905` 条指令，`CRC32 3914993999`，SHA-256 `bb3250bc6ee5f40545811e9c7f70e8c8528255751c03a9ceeda06cfbf31d01dc`）。
   - 重装配并原子同步 `huaBin/`（`5,129` 个清单文件，`bin/hua.exe` `981080c3...`、`hua-generator.exe` `407beb66...`、`hua-archive.exe` `c9cd6a57...`、`compiler.huab` `31044ed1...`）。
2. **仓库外多包 CLI 验收（`python tests/local_package_cli_tests.py --sdk huaBin`）**：
   - 在仓库外全新临时目录创建含中文空格路径的三包工作区：`audit_cli`（`main.hua`、`config.hua`）依赖外部包 `logschema`（`package.hua`、`rules.hua`）与 `reportfmt`（`package.hua`，同时依赖 `logschema` 构成菱形依赖）。
   - **正例与脱离源码执行**：`hua run`、`hua run --release`、移走全部源码目录后的宿主 `.huab`、自举 `.huab`（`format 29`）与独立 `audit_cli.exe`（`2,341,888` 字节，SHA-256 `3ca23b8aa49c9ad8...`）五路标准输出均为 `Prod Gateway: kept=3/4 latency=1,250ms (inits=1)`（验证菱形依赖仅初始化 1 次），原子写出的 5 份美化 JSON 文件逐字节相同。
   - **跨包错误位置与退出码一致**：非法 JSONL 记录触发 `std.error.Value` 跨包 `?` 传播，`hua run`、双端无源码 `.huab` 与独立 `.exe` 四路退出码均为 `1`，`stderr` 逐字节完全一致，精确包含 `main.hua:65:1`、`@pkg/logschema/rules.hua:30:16`、`@pkg/logschema/package.hua:37:18`、`main.hua:80:19`；未知命令行参数触发 `hua.cli/usage` 退出码均为 `2` 且 `stderr` 一致。
   - **N3 外部包缓存与 36 项负例诊断对照**：未改源码复编命中 `(0, 0, 0)`；修改外部包 `@pkg/logschema/rules.hua` 后自动触发重生成（`generate_calls=1`）。9 类包解析错误（缺失目录、缺 `package.hua`、`std` 保留名、清单名不匹配、本地同名冲突、传递菱形异目录冲突、`\u0070` 重复键、包清单循环、包内模块循环）× 4 入口（`hua check`/`hua run`/`hua build --release`/`hua-archive.exe`）共 `36/36` 检查全部通过且不残留坏产物。
3. **回归验证**：
   - 宿主与模块/诊断/标准库定向 CTest：`frontend`、`entry_boundary`、`entry_contract`、`vm_modules`、`cli_contract`、`module_contract`、`diagnostic_source`、`diagnostic_contract`、`capability_dependencies`、`error_foundation`、`algorithms_config`、`filesystem_stdlib`、`file_stdlib`、`file_bulk`、`fs_walk_copy`、`os_exit`、`strings_p0`、`strings_template`、`json_construct` 共 19 项通过（`selfhost_archive` 仍属既有 `HUA-D036` 30 秒探针时限问题；`unicode_case` 因本机 Python 3.12 Unicode 版本与表差异属环境对照差异）。
   - 四套仓库外 SDK 回归：`windows_sdk_stdlib_tests.py`、`windows_release_sdk_tests.py`（含候选编译器修改源码重建）、`windows_native_run_tests.py`（热运行中位 `223.7–224.0 ms`）、`windows_release_cache_tests.py`（28 项缓存与增量失效场景）全部通过。

## 5. 结论与下一步

第 1 步（仓库外多包 CLI + 标准库逐接口覆盖基线）已按确认合同实现并验收通过，未提交/未推送 Git。下一步进入**第 2 步：建立可重复的构建和双端标准库单一权威合同**（单源生成/校验宿主与自举签名及版本门禁，并按批次继续扩大双端标准库覆盖）。
