# 2026-11-27 第 5 步规划、全景脱钩 C++ 路线、`huaBin` 定位与汇编重写 VM 深度分析

## 1. 第 5 步详细规划（5A–5E）
- **5A**：纯 Hua `std.csv`（基于 `std.stream.Reader/Writer`，RFC 4180 流式读写）+ 轻量配置解析；验收样例为 CSV/JSONL 清洗导入 SQLite CLI。
- **5B**：线性时间 NFA `std.regex` + 流式 `gzip/deflate` 装饰器；验收样例为 `.log.gz` 审计入库 CLI。
- **5C**：`std.process.spawn` 流式子进程管道（`stdin: Writer`、`stdout/stderr: Reader`、超时终止与回收）；验收样例为本地 MCP stdio / 构建驱动工具。
- **5D**：剩余 84 项标准库双端清零（5D-1 `random` 句柄 + `--release` 原生 `async/spawn` 14 项；5D-2 容器多态 53 项；5D-3 高阶回调 19 项），达成 30 内建模块 `406/406` 四路径 100% 全通。
- **5E**：Python Bridge 作为可选 SDK 包装配验收；`std.sql` 待第二个真实数据库驱动出现后再抽取。

## 2. 现状盘点与 C++ 依赖三道锁（2026-11-27 实核）
- 仓内 C++ 共 `108` 文件 / `25,616` 行；`selfhost/*.hua` 共 `36` 文件 / `11,124` 行；`std/*.hua` 共 `2` 文件 / `176` 行。
- 三道依赖锁：(1) 可退役的历史 C++ 前端脚手架（~9,500 行，待 `selfhost/` 补齐固有方法/`interface`/泛型/`@test` 后统一切换）；(2) `--release` 中间输出 C++20 文本依赖随包 Clang/lld；(3) GC/Task/VM/OS 底层运行时（~16,000 行）依赖裸内存与 OS API 调用。
- 脱钩三阶段：阶段 1 前端大一统 + 纯 Hua 标准库 + 重组件外置 ABI v2 包 → 阶段 2 自研轻量 x86_64/ARM64 机器码与 PE/ELF/Mach-O 后端摘除 Clang++ → 阶段 3 仅对 `@sdk/src/runtime/` 开放受控内部系统方言，用 Hua 重写 GC/调度器/VM 实现源码仓 0 行 C++。

## 3. `huaBin/` 核心定位
- 对外：零前置依赖、跨机器解压即用的官方便携 SDK。
- 对内：语言脱离 C++ 后跨代自我繁殖的活体种子库（Living Seed / Stage-0 Anchor）。
- 纯化终极标尺：`huaBin/` 不再包含 `include/*.hpp`、`libhua_semantics.a`、`toolchain/clang++`，且 `bin/hua.exe` 本身由 `src/` 下 `.hua` 源码直接编译产出。

## 4. 汇编重写 VM 底层评估
- **收益**：纯标量微基准 `1.8×–3.0×`，真实编译/容器负载仅 `1.05×–1.20×`（分发仅占 ~7.2%–12%，值复制/COW/分配/类型检查/GC 占 >75%），且无法逼近 `--release`（`7.8×–21×`）。
- **难点与跨平台**：需先去除 C++20 协程帧、手写 Win64 SEH / Linux DWARF / Apple ARM64 Compact Unwind 异常展开表、紧耦合 48B `Value` 布局、同时维护 Win64/SysV/ARM64 三套汇编（iOS 禁 JIT 但允许静态 AOT 汇编解释器，禁用 `x18`）。
- **推荐替代**：(1) Clang 23 `[[clang::musttail]]` 尾调用线索化改造 `register_vm.cpp`（零跨平台维护成本，获 85%–90% 分发收益）；(2) `TypeId` 整数驻留 + 扩大 `register_vm` 覆盖；(3) 远期布局彻底冻结后再评估单源 OfflineASM 风格生成器。
