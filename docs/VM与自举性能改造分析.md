# VM与自举性能改造分析

更新于 2026-11-18。本文维护原生发布路线、自举提速计划和支持排序的实测结论。两条路线分别验收，目前都从 Windows 出发。完整实验过程见[项目记忆索引](../.codex-sync/knowledge/bootstrap-performance/INDEX.md)。

## 0. 当前进度

### 0.1 当前状态

- **原生发布路线**进入 N2 并发与异步。N3 发布部署、N4 统一带类型 IR 与原生自举、N5 扩展能力依次排在后面。
- **自举提速路线**待做完整负载分阶段归因。先确定 C04 的重复分析、摘要展开和合并成本，再做调用点精确失效与按需查询。VM 内存成本和归档成本由测量结果决定是否进入实施。

### 0.2 最近的 Windows 验收基线

N1 的 Windows 完整自身复验和同步动态操作审查已完成。87 项原生／VM 差分与 12 项拒绝检查通过。下表来自 2026-11-18 的验收，输入均为完整自举编译器入口 [selfhost/archive_main.hua](../selfhost/archive_main.hua)。

| 环节 | 墙钟时间 |
|---|---:|
| 宿主生成完整编译器 C++ | 78.187 秒 |
| Clang 编译并链接原生编译器 | 163.438 秒 |
| 宿主构建 VM 种子 HUAB | 76.275 秒 |
| 已构建的原生编译器编译自身，输出 HUAB | **48.969 秒** |
| VM 加载种子编译同一输入，输出 C2 HUAB | **308.588 秒** |
| VM 加载 C2 再编译同一输入，输出 C3 HUAB | 319.607 秒 |

原生和 VM 跑的是同一套 Hua 编译器逻辑。原生程序由带类型 AST 生成的 C++ 经 Clang 构建，运行时关闭计步。VM 执行种子中的字节码，使用 compiler 预算配置。48.969／308.588 秒各为一次墙钟实测，约 6.30 倍，包含运行与产物写出，各自的种子／可执行文件准备时间单列。

原生产物、C2 和 C3 逐字节一致。HUAB 格式 7、ABI 1，4,195,418 字节、62,599 条指令。C2 执行 1,132,389,837 个预算步，峰值调用深度 52。两条执行路径相差 259.619 秒，目前缺少各阶段及运行时热点的同期归因。N4 仍需完成由 Hua 源码生成原生编译器的闭环。[Windows 验收记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-18-n1-windows-acceptance.md)

### 0.2.1 Windows 跨语言基准（2026-11-18）

同机对照 Lua 5.4.7、CPython 3.12.0、Java 25.0.2、Go 1.27.1 与 Clang 23.1.2 的 C++。七组端到端用例包括空程序、循环、递归、Map、文本、整数容器和 4 MiB 文件。每格 7 次，所有计入统计的输出校验通过。

- **递归 fib(30)**：Hua HUAB 115.42 ms、原生 24.76 ms，Lua 79.12 ms、Python 266.67 ms、Java 111.61 ms、Go 20.92 ms、C++ 15.60 ms。AST 首次达到 60 秒即终止，保留已完成样本并跳过后续 AST 项。
- **预热递归**：三次不同参数计算中，Hua HUAB 103.56 ms、Hua 原生 8.61 ms、Lua 75.23 ms、Python 209.76 ms、Java 5.67 ms、Go 5.60 ms、C++ 3.34 ms。
- **30 万项整数容器**：HUAB 589.60 ms／58.9 MiB，原生 27.86 ms／10.8 MiB。后续 VM 剖析可用此例分离值、List 调用与分配成本。自举性能仍以完整输入归因为准。

[完整时间、峰值工作集、编译数据与边界](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-18-cross-language-benchmarks.md)

<a id="03-下一步"></a>

### 0.3 下一步

<a id="native-release-route-20261114"></a>

#### 原生发布路线 · N2 到 N5

沿用 2026-11-14 已确认的路线。发布后端通过带类型 AST 生成 C++，使用随发行包提供的 Clang／lld。原生发布默认关闭计步，可选在循环回边及调用处检查预算。VM 继续承担快速开发、语义对照、带预算的沙箱及嵌入。其他平台验收暂缓，N2 优先 Windows。

| 阶段 | 实施顺序与交付 | 验收 |
|---|---|---|
| **N2 并发与异步** | 线程池上的栈式协程，采用 M:N 调度。先接 Hua 的任务创建、`await`、取消及超时，再接 Windows IOCP 与同一调度器的事件循环。跨 Task 传递按已定规则做深快照，资源按现行所有权及在途保活规则处理。 | 与 VM 对照返回值、错误、取消和超时。覆盖单次恢复、迟到完成、资源清理、等待不占用计算线程，实测多核并行。 |
| **N3 发布与部署** | 提供 `hua build --release` 单文件 Windows 可执行程序，打包 Clang／lld 和预编译运行时。按模块分编译单元，以模块和依赖的身份做增量缓存（HUA-D021），Wasmtime 按需链接。 | 可执行文件可独立运行。测量干净构建、缓存命中及依赖变更后的重建。原路线目标为小程序构建低于 2 秒、约 1 MB 级，自举编译器的增量构建有可重复的改善。 |
| **N4 统一带类型 IR 与原生自举** | 从带类型 AST 降到统一 IR，再从 IR 分别生成字节码和 C++（HUA-D009）。按实例引入逃逸分析、冗余边界检查消除及跨模块内联。逐步将 C++ 生成器移入 `selfhost/`，完成原生自举闭环。 | 同源 HUAB 可复现，字节码与原生程序行为、诊断、退出码通过差分。用 Hua 编译器生成下一代原生编译器，再由下一代完成同源编译并验证产物与行为。 |
| **N5 扩展能力** | VM 嵌入 C API、稳定的扩展 C ABI、WASM 发布、HTTP／JSON 网络应用、parallel／SIMD、AI／Agent 运行时。按能力族推进。 | 各族分别确定接口、兼容和资源合同及验收输入。 |

N2 涉及的任务恢复、取消、预算和资源协议以[运行时与扩展设计](运行时与扩展设计.md)为准。每项原生能力用 `tests/native_backend_tests.py` 对照 VM 的标准输出、诊断及退出码。子集外输入须在生成 C++ 前给出明确诊断并以退出码 2 拒绝。`tests/native_selfhost_tests.py` 提供小规模双后端对照，完整自身验收另按第 2 节执行。

<a id="selfhost-performance-plan"></a>

#### 自举提速路线 · 先减少重复分析，再减少执行成本

1. **在完整自身输入上建立分阶段基线。** 固定源文件清单及内容、构建身份、工具链和预算。原生和 VM 分别记录解析／名称分析、类型分析、C04 能力分析、降级、指令生成、栈及产物校验、HUAB 编码、文件发布。记录每段墙钟，VM 同时采集阶段预算步数。嵌套阶段注明包含关系。C04 记录函数体访问数、摘要实际变化数、调用点展开的来源条目数、合并时检查及改变的字段数。诊断计数及 CPU 采样与发布计时分别采集。
2. **先做 C04 调用点按实际依赖失效。** 现有被调用方优先顺序与反向依赖表为起点。调用点记录实际读取的返回来源、写效果或别名等摘要部分，按对应事实的版本判断是否失效。摘要变化只唤起依赖相应变化的调用点及其所在分析单元。递归、未知别名、未知写入和无法证明依赖完整的情况沿用保守重算。验收减少的函数体访问、工作量及耗时，同时检查 A07 只读、别名、写效果与跨函数传播。
3. **再让摘要共享来源关系并按需投影。** 用参数编号、字段路径 ID 与能力标志存共享的不可变关系。调用时绑定实际参数，查询命中具体字段才投影该条路径，稀疏覆盖保存差异。首片只覆盖高频、规则明确的参数字段投影，其他情况保守回退。记录来源展开、字段遍历及临时对象数量。历史数字事实候选的逐字段解码及过滤抵消了收益，当前以真实负载中的工作量下降验收。[数字事实候选](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-20-c04-direct.md)
4. **按剩余热点选后续改造。** 同一阶段若仅 VM 明显较慢且复制、分配及 GC 仍集中，先对采样点名的结构试做整个不可变结构共享、写入时分离，同时核对可写别名、已发布位置、Task 隔离、GC 计数和回收。若双后端的归档阶段均占明显份额，优先复用校验或生成阶段的格式元数据，合并重复扫描，对连续字段批量编码。每个改动独立对照。紧凑指令、寄存器 VM 或更换 GC 策略由后续剖析和合同核定决定。

### 0.4 Windows 诊断入口

使用本机已构建的 Release `hua.exe`，按实际构建目录调整路径。

```powershell
python perf/run_bench.py --hua tests/build/hua.exe
python perf/ab_bench.py --base tests/build-baseline/hua.exe --new tests/build/hua.exe --repeat 7
```

指令及分配计数使用 `HUA_INTERNAL_PROFILE` 构建与 `perf/count_bench.py`。CPU 采样入口在 `tests/tools/native_profile/`。完整自举的入口、工具链和产物目录见[开发与运行指南](开发与运行指南.md)及[N1 验收记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-18-n1-windows-acceptance.md)。

<a id="9-基于真实负载剖析的优化路线2026-10-22"></a>

## 1. 用于排序的性能证据

| 观察 | 对当前方案的意义 |
|---|---|
| 旧版完整自举跟踪在 5 亿步处截断，C04 已执行 3.61 亿步。调用点来源实例化、来源合并、路径前缀传播各消耗约 4,225 万、2,799 万、2,406 万个独占步骤。 | 当前应测实函数体重访和摘要传播。该截断跟踪说明历史热点，当前完整执行的阶段占比仍待测。[历史跟踪](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-20-direct-selfhost-trace.md) |
| 被调用方优先的同机同输入 A/B 把步数从 15.70 亿降至 12.76 亿，墙钟从 228.6 秒降至 184.3 秒，产物相同。当前代码已采用该访问顺序。 | 改变分析工作量已带来约 19% 的全流程收益。当前切片针对现有顺序下剩余的失效和传播成本。[历史 A/B](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-23-p4-s1-callee-first.md) |
| 历史 `ast_pool` 真实编译样本中约有 174 万次结构复制、17 万次字段写入。采样中复制约 28%（含子调用）、GC 约 11%、分发自身约 7.2%、名字查找约 1.9%。 | 优先验证值复制和对象存活成本。采样组有重叠，小型真实负载比例应与完整自身归因分开。[底层剖析](../.codex-sync/knowledge/bootstrap-performance/designs/2026-11-08-vm-lower-layer-review-proposal.md) |
| 叶记录字段体共享降低约 15.5% 的累计申请字节，但 GC 扫描对象约增 19%，真实编译基本持平。作用域复用让算术用例快 1.46 倍，`ast_pool` 为 1.00 倍。 | 同时观测分配次数、GC 扫描与真实负载墙钟，防止搬移开销。[字段体共享](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-29-value-snapshots.md) · [作用域复用](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-07-s2-2a.md) |
| 当前归档会扫描指令判定格式，写出时再次遍历并逐字段调用 Buffer。 | 先用归档单独计时决定批处理优先级。[归档实现](../selfhost/archive.hua) |

## 2. 测量与验收

1. **对照负载。** 完整 `selfhost/archive_main.hua` 是决策基准。`ast_pool` 与 `type_rules` 用于快速筛查，适用的同机 A/B 交替运行。记录版本、源文件身份、构建参数、是否计步、墙钟及必要的工作量计数。分开记录准备种子、生成 C++、Clang 构建与运行编译器的时间。
2. **同源产物及诊断。** 新旧编译器处理同一冻结输入，核对 HUAB 字节、校验和、独立 reader、回执、诊断及退出码。对非法输入确认能力检查拒绝且不发布产物。运行相关 A07/C04 正反例及 AST、源码 VM、无源码 HUAB 对照，同步原生路径加入差分。算法优化记录预期减少的预算步数，运行时切片核对原逻辑计步和故障位置。
3. **代际验收。** 对更新后的编译器源码重新生成种子，完整运行 C2、C3，核对同源 C2=C3，在隔离副本上移走编译器源码后，验证已有 HUAB 仍可加载并编译示例。原生编译器输出的 HUAB 与同源 VM 产物比较。
4. **预算与放行。** 完整自身每个受监督进程的墙钟上限为 600 秒，VM compiler 配置为每个执行器 1000 亿步、调用深度 128。首次严重超时即停止该验收链路，保留最后阶段、步数和诊断。候选需在真实负载上产生可重复的工作量和耗时改善，独立记录语义边界及退化，避免按单个微基准放行。

## 3. 历史记录索引

<a id="execution-storage-priority2"></a>
<a id="route-reorder-20261108"></a>
<a id="s2-2a-results-20261107"></a>
<a id="s2-1-results-20261030"></a>
<a id="value-copy-priority1-results"></a>

- [执行存储试验与否决](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-30-s2-1.md)、[值复制结果](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-29-value-snapshots.md)、[作用域复用与绑定槽](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-07-s2-2a.md)和[整体结构写时复制设计](../.codex-sync/knowledge/bootstrap-performance/designs/2026-11-08-v1-struct-cow-design.md)。旧执行存储顺序及其估算以历史方案留存，本轮决策使用 0.3 的两条路线。

<a id="p4-s12026-10-23windows另一台机器c04摘要轮次按被调用方优先访问"></a>

- [C04 被调用方优先 A/B](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-23-p4-s1-callee-first.md)。

<a id="windows完整ctest回归2026-10-22"></a>

- 2026-10-22 Windows CTest 逐项分析、旧 P／S 与 A–G 路线、过往 N1 实施细节见[整理前原文](../.codex-sync/knowledge/bootstrap-performance/originals/2026-11-18-VM与自举性能改造分析.md)。归档原文中的相对链接以原 `docs/` 路径为基准。
