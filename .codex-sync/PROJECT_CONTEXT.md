# Hua项目背景

更新2026-10-22。项目：NssSSLY/Hua（https://github.com/NssSSLY/Hua.git，分支main），project_id=aab00f84-5a35-48b3-973b-4ccd29e3d31c。本机路径C:/file/project/hua，其他电脑路径可不同。

Hua是小核心、静态类型的脚本语言：C++20宿主（Lexer/Parser/语义检查/AST解释器/字节码VM/GC/HUAB产物）＋用Hua写的编译器（selfhost/，14模块，入口selfhost/archive_main.hua）。路线：语言基础A → 基础标准库B → 编译器自举C → 应用扩展与高级优化。

**现状**：Hua编译器已在Windows上编译自身并到达代际不动点（C2=C3逐字节相同），2026-10-22同时段约344秒（compiler配置600秒墙钟为主、步数上限1000亿）。当前工作是性能（拉开余量、接近成熟解释器）与总验收（C09覆盖/兼容、A12/B10、G1–G3），不是“能不能自举”。

**权威入口**（共10份活文档，见docs/文档目录.md）：
- 进度/能力：docs/项目概览与进度.md
- 任务/优先级/唯一下一方案：docs/设计决策与未决事项.md（design-backlog，固定HUA-D编号）
- 性能分析、A–G路线与按步骤工作日志：docs/VM与自举性能改造分析.md（第0节日志，0.3节下一步，第9节路线）
- 语言合同：docs/语言设计与类型规则.md；运行时：docs/运行时与扩展设计.md；接口：docs/标准库与内建接口.md
- 构建/运行/在另一台电脑接续：docs/开发与运行指南.md#resume-on-another-computer
- 逐项学习与确认：docs/语言百科与设计核对.md（固定条目ID，用户确认栏不改）

**目录约定**：构建在tests/build（默认）或tests/build-*/（均不入Git）；原始日志tests/artifacts/（不入Git）；诊断工具tests/tools/；性能基准perf/（含reference/的CPython与Lua参照）；本机工具.tools/（不入Git：llvm-mingw-20260922-ucrt-x86_64、wasmtime-v49.0.2-x86_64-windows-c-api、可选lua-5.4.7）；完整过程记录.codex-sync/knowledge/（性能入口knowledge/bootstrap-performance/INDEX.md）。docs/history为原字节归档，不改写。

**工作规则**（详见AGENTS.md）：项目文档用中文；一个任务只更新涉及的文档；区分规划/实现/已验证；提交与推送只在用户明确要求时做。
