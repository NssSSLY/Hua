# Hua项目背景

2026-11-25 在第二台 Windows x64 实体机完成 C04 改造后的 P0 收尾：恢复 3 项 C04 白盒单测，重装配含 `origin_pool.hua` 的 15 模块官方 `huaBin` 与便携 ZIP，验证跨实体机 `C2 == C3` 逐字节一致（`566b7a69...`）及四套仓库外 SDK 验收通过。[记录](knowledge/bootstrap-performance/runs/2026-11-25-p0-c04-tests-and-huabin.md)

2026-11-24 huaBin 已支持 Windows 一条命令原生构建并运行 Hua 源码。可以直接运行源码 VM，也可发布独立 EXE。[记录](knowledge/bootstrap-performance/runs/2026-11-24-local-run.md)


2026-11-24 Windows N3.5 小程序原生发布和 ZIP 可搬移性已在本机验收。Hello 和标量两模块冷构建中位 0.784／0.798 秒，EXE 约 0.46 MiB。带容器两模块中位 1.960 秒、最大 2.013 秒，EXE 为 1.84 MiB。正式 ZIP 通过本机搬移，四类工作负载完成原始 PE 字节复现。[实测](knowledge/bootstrap-performance/runs/2026-11-24-n3-5-windows-release.md)


2026-11-23 Windows N3.4 增量发布。完整原生生成器首次编译 13 个对象，用时 385.679 秒。仅修改词法器实现后重编 `lexer.hua` 一个对象，用时 70.031 秒。修改实际 `Token` 布局并由 Parser 读取新字段时只重编 Lexer 和 Parser。[记录](knowledge/bootstrap-performance/runs/2026-11-23-n3-module-objects.md)



2026-11-23 Windows N3.3 已验收。本地 `huaBin` 已更新。接续 N3.4 模块对象与依赖失效。[记录](knowledge/bootstrap-performance/runs/2026-11-23-n3-native-cache.md)

2026-11-23 Windows 原生 `huaBin` 已支持 `hua build --release`、原生 HUAB 编译、独立应用分发和用 Hua 源码重建候选生成器。[验收](knowledge/bootstrap-performance/runs/2026-11-23-n3-native-sdk.md)


2026-11-18 用户确认现阶段以 Hua 自身扩展语法、标准库和产物生成能力为主，逐步收缩 C++ 实现职责。Windows huaBin 已按可搬移 SDK 交付，仓库外样例、缺依赖和 C2/C3 验收通过。[证据](knowledge/bootstrap-performance/runs/2026-11-18-windows-seed-sdk.md)实现、测量与 N2/N3/N4 的次序见[总规划](../docs/设计决策与未决事项.md#hua-owned-toolchain)。标量 async/await 已通过旧种子重建及两代验收。P0 已完成源码标准模块接入与完整阶段归因。C04 是后续性能工作的首要热点。[验收](knowledge/bootstrap-performance/runs/2026-11-18-selfhost-scalar-async.md)

2026-11-18 原生发布进入 N2.3。Windows 第一个 Task 里程碑采用单计算线程和标量跨 Task 边界，当前验收与独立的自举工作见 CURRENT_STATE 和 TODO。

2026-11-05收尾：用户确认的整数/双曲24接口已交付，since22，26模块332函数；详见CURRENT_STATE和2026-11-04-p1-math-arithmetic运行记录。上一批6接口保持。

2026-11-04用户确认并实现HUA-D038 P1 Set对称差＋math五个位工具，since21，26模块308函数；功能验收通过，07_strings源模式约6%性能保留，详见当前状态和运行记录。


## 2026-10-30本轮接续

2026-10-30：当前 main@776d5f8 的 C07 正式自身闭环已验收。14模块/291088字节，210.844秒、1281255218步、peak_calls 52，C2 4195418字节、CRC32 3424058752；独立reader、编译器源码移除、生成行为和示例源码移走执行均通过。本轮未改生产源码/预算，未重跑C08或全量CTest，未提交推送。

详见[正式记录](knowledge/bootstrap-performance/runs/2026-10-30-c07-acceptance.md)。

更新2026-10-22。项目：NssSSLY/Hua（https://github.com/NssSSLY/Hua.git，分支main），project_id=aab00f84-5a35-48b3-973b-4ccd29e3d31c。本机路径C:/file/project/hua，其他电脑路径可不同。

Hua是小核心、静态类型的脚本语言：C++20宿主（Lexer/Parser/语义检查/AST解释器/字节码VM/GC/HUAB产物）＋用Hua写的编译器（selfhost/，14模块，入口selfhost/archive_main.hua）。路线：语言基础A → 基础标准库B → 编译器自举C → 应用扩展与高级优化。

**现状**：Hua编译器已在Windows上编译自身并到达代际不动点（C2=C3逐字节相同），2026-10-22同时段约344秒（compiler配置600秒墙钟为主、步数上限1000亿）。当前工作是性能（拉开余量、接近成熟解释器）与总验收（C09覆盖/兼容、A12/B10、G1–G3），不是“能不能自举”。

**权威入口**（共10份活文档，见docs/文档目录.md）：
- 进度/能力：docs/项目概览与进度.md
- 任务/优先级/唯一下一方案：docs/设计决策与未决事项.md（design-backlog，固定HUA-D编号）
- 原生发布与自举性能：docs/VM与自举性能改造分析.md（0.3 节为 N2–N5 与自举提速，附当前证据和验收方法，历史过程在 knowledge/bootstrap-performance/）
- 语言合同：docs/语言设计与类型规则.md；运行时：docs/运行时与扩展设计.md；接口：docs/标准库与内建接口.md
- 构建/运行/在另一台电脑接续：docs/开发与运行指南.md#resume-on-another-computer
- 逐项学习与确认：docs/语言百科与设计核对.md（固定条目ID，用户确认栏不改）

**目录约定**：构建在tests/build（默认）或tests/build-*/（均不入Git）；原始日志tests/artifacts/（不入Git）；诊断工具tests/tools/；性能基准perf/（含reference/的CPython与Lua参照）；本机工具.tools/（不入Git：llvm-mingw-20260922-ucrt-x86_64、wasmtime-v49.0.2-x86_64-windows-c-api、可选lua-5.4.7）；完整过程记录.codex-sync/knowledge/（性能入口knowledge/bootstrap-performance/INDEX.md）。docs/history为原字节归档，不改写。

**工作规则**（详见AGENTS.md）：项目文档用中文；一个任务只更新涉及的文档；区分规划/实现/已验证；提交与推送只在用户明确要求时做。
