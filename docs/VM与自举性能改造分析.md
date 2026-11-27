# VM与自举性能改造分析

更新于 2026-11-25。本文维护原生发布路线、自举提速计划和支持排序的实测结论。两条路线分别验收，目前都从 Windows 出发。完整实验过程见[项目记忆索引](../.codex-sync/knowledge/bootstrap-performance/INDEX.md)。

## 0. 当前进度

### 0.1 当前状态

- **原生发布路线**在 Windows 已通过 N2.1–N2.5 与 N3.1–N3.5，并已在第二台独立实体机完成更新后 15 模块 `huaBin` 搬移、跨机字节复现、`hua run --release` 与增量缓存复验。覆盖任务快照、清理、控制、多 worker、File IOCP、整次发布缓存、按模块增量发布与便携 ZIP 发行。Hua 自举已覆盖可快照记录和捕获外层变量的局部异步递归。
- **自举提速路线**已完成 C04 能力分析算法改造（50-bit 内联单元素分解式短 ID 来源池 `selfhost/origin_pool.hua`、模块只读操作表 `UnitPlan`、按字段返回需求 `demand`、按类型 `alias_gen` 与批量别名缓存、Kosaraju 自底向上 SCC 局部不动点求解），已落盘 `selfhost/` 与 `huaBin/` 并通过双端同输入验收、206 项 A07/C04 差分、9 项 C04 白盒/契约单测及两台实体机一致的 15 模块 `C2 == C3` 固定点。非 C04 阶段（`325.4M` 步）现已超过 C04（`299.3M` 步），后续可按新剖析决定非 C04 阶段或 VM 值/GC 优化。

### 0.2 最近的 Windows 验收基线

完整编译阶段对照已完成。VM 为 586.797 秒，原生为 83.390 秒。C04 能力分析占 VM 阶段墙钟 88.52%，占原生 94.14%。两条路径产物与验收 C3 逐字节一致。[分阶段实测](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-18-full-selfhost-phase-profile.md)

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

<a id="native-n2-first-milestone"></a>

### 0.2.2 Windows 原生 Task 首个里程碑

N2.1 隔离执行状态及标准库调用数据。N2.2 用 Windows 纤程保存原生栈，复用已有 TaskRuntime 的队列、定时和完成交付。验收覆盖父子物理线程一致、两层普通函数内等待、完成与登记两种顺序、重复等待、字符串快照、异步主函数和错误诊断。[运行时边界](运行时与扩展设计.md#native-task-single-worker)

同步循环、递归与任务定时对照见[实测记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-18-n2-single-worker.md)。

<a id="native-n2-container-tasks"></a>

### 0.2.3 N2.3 容器快照

原生和 Hua 自举前端接受只读标量叶的 Slice、定长数组、List 和 Map。任务启动前递归复制参数，保留原生带类型表示，避免参数经过通用 Value 再拆箱。每次等待沿用 TaskRuntime 的结果深复制，等待方可独立修改结果。同步编译器 C++ 的同源字节对照与本机 SDK 复验见[本轮记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-18-n2-container-tasks.md)。

<a id="native-n2-windows-workers-iocp"></a>

### 0.2.4 N2.4 与 N2.5 Windows 实测

原生固定 8 个任务和 2,000 万轮计算。1／2／4 worker 的中位耗时为 95.24／54.60／37.80 毫秒。实际使用了规定数量的 worker，各任务挂起前后保持同一线程。

128 MiB 文件读取包含进程启动、打开和关闭。7 次交替对照中，阻塞池到 IOCP 的中位耗时从 31.35 降至 25.88 毫秒，端到端样本 P95 从 85.82 降至 41.85 毫秒。峰值线程 16→17，峰值工作集 9.80→10.07 MiB。数据来自本机文件缓存负载。AST、源码 VM、宿主 HUAB、Hua 自举 HUAB 和原生文件内容一致。[本轮记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-18-n2-windows-workers-iocp.md)。

### 0.2.5 N2 Task 收拢省去废弃副本（2026-11-20）

作用域和任务组退出、all / race 收尾可跳过常见数据树的深复制，继续等待终态、输出和诊断。显式 await 仍获得独立结果。Windows 同机每个 Task 75,000 行、四个 Task 的端到端中位数为 HUAB 949.29 → 616.30 ms，原生 679.85 → 342.55 ms，分别下降 35.1% 和 49.6%。峰值工作集分别下降 40.68 和 30.44 MiB。[复现与验收记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-20-n2-task-join.md)

### 0.2.6 N2 按需模块快照和完成投递（2026-11-20）

32 个短任务只读一个模块槽的负载，中位数 25.83 → 15.31 ms。频繁 yield 负载在 1／4 worker 下分别为 138.33 → 67.58 ms 和 127.70 → 66.84 ms。4 KiB 缓存读相同 worker 数的改善约 3%／5%。Task 已开放 Bytes、Optional 和 Result 的首片。其后普通局部闭包的自举缺口已修复并完成四路径复验。本组数据来自先前候选。[当轮记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-20-n2-followup.md)

<a id="native-n2-final-20261120"></a>

### 0.2.7 Windows N2 整体验收（2026-11-20）

七个程序在五条执行路径上一致。108 个原生与 VM 差分及 13 个生成前拒绝通过。真实在途读取消只完成及退休一次。C2／C3 分别为 247.777／251.964 秒，两代逐字节相同，各通过 14 正例与 17 反例。

用最终宿主运行 1／2／4 worker 固定计算，中位数 134.25／98.49／66.46 ms。128 MiB 缓存读取中，IOCP 比阻塞池中位耗时低 21.5%。大结果收拢端到端中位数在原生下降 49.9%，在 HUAB 下降 32.1%。按需模块快照和原生频繁让出在同源 A/B 继续验证有改善。[Windows N2 验收](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-20-n2-final.md)

<a id="03-下一步"></a>

### 0.2.8 Windows N3.3 原生发布缓存（2026-11-23）

Windows N3.3 已验收。相同输入整次复用 EXE，生成内容相同可复用对象，仅运行库变化可复用对象并重新链接。完整原生生成器冷构建 415.461 秒，删除输出后 0.529 秒恢复同哈希 EXE，生成、编译与链接均跳过。Hello 冷／热为 5.247／0.404 秒。首次逐文件扫描 SDK 的校验成本为 24.730 秒，改用 NTFS 批量标识后，完整热构建的输入校验为 0.388 秒。[记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-23-n3-native-cache.md)

### 0.2.9 Windows N3.4 按模块增量发布

Hua 生成器为逻辑模块、命名声明和枚举成员构造与 Core 全局计数解耦的标识。每个单元的接口包含实际引用的函数、全局声明和具名类型，以及布局的传递依赖。类型复制与装箱函数由定义模块提供。多模块程序由启动单元显式按 C05 顺序执行初始化，单模块程序合并启动代码。

整次 EXE 检查仍优先。未命中时重新进行全程序语义分析和降级，按模块 C++ 与其生成接口内容复用对象，按有序对象摘要和工具链链接。工具链指纹在构建期间变化时，构建失败并保留上次产物。单文件协议用于旧生成器兼容与同机对照。

完整原生生成器首次编译 13 个对象，用时 385.679 秒。仅修改词法器实现后重编 `lexer.hua` 一个对象，用时 70.031 秒。修改实际 `Token` 布局并由 Parser 读取新字段时只重编 Lexer 和 Parser。 整次命中恢复同哈希 EXE 用时 0.564 秒。[验收](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-23-n3-module-objects.md)

同机同源码和同一生成器下，词法器改动的单翻译单元构建为 398.737 秒，模块增量为 70.031 秒，加速 5.69 倍。采样进程树峰值从 2,126 MiB 降至 234 MiB。增量与干净分模块构建的 EXE 同哈希。两模块小程序无缓存构建中位数从 3.622 秒增至 8.754 秒，首次构建继续由 N3.5 优化。

### 0.2.10 Windows N3.5 原生发行

Hello 和标量两模块冷构建中位 0.784／0.798 秒，EXE 约 0.46 MiB。带容器两模块中位 1.960 秒、最大 2.013 秒，EXE 为 1.84 MiB。正式 ZIP 通过本机搬移，四类工作负载完成原始 PE 字节复现。[实测](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-24-n3-5-windows-release.md)

### 0.2.11 C04 能力分析算法改造（2026-11-24）

以 `349b79c` 冻结的 14 模块 `archive_main.hua` 闭包为同输入对照（未改 C++ 运行时，普通计时与独立分阶段计时分离）：

- **因果归因**：基线 C04 占 VM `934.62M / 1,260.05M` 步（`74.17%`），纯无效重访仅 `14.40%`（单改失效上界 `1.17×`），`48.59%` 为仅来源集合增长导致的宽集合展开与合并，Top-3 函数（`capabilities.statement`、`expression_body`、`analyze`）占 `71.77%`。
- **核心改动**：(1) 新增 `selfhost/origin_pool.hua`，用 50-bit `View.oid` 将参数来源 `poid`、局部站点 `lsite` 与规范局部集 `loid` 分解编码并内联单元素集合，使调用点局部重标由逐原子展开降为 O(1) 整数运算并大幅压低堆 `map` 数量；(2) `selfhost/capabilities.hua` 一次性提取每模块只读操作表 `UnitPlan`（`[]NodeOp`）与预缓存标准库签名；(3) 实现按字段返回需求 `demand`（丢弃 / 全量 / 单字段 `#@field`）、按类型版本化 `alias_gen` 与批量别名缓存、`note_prefix` O(1) 去重与纯标量调用快径；(4) `analyze()` 改为 Kosaraju 自底向上 SCC 局部不动点求解，保留 32 轮保守回退、首错位置与预算语义。
- **普通无埋点双端实测**（HUAB 逐字节一致 `db6dcfcc3d8f0ad1...`）：
  - 原生整次编译：`52.256s → 16.822s`（**`3.11×`**，CPU `16.031s`，峰值 `175.38 → 128.22 MiB` 降 `26.9%`）。
  - VM 整次编译：`266.516s → 130.835s`（**`2.04×`**，CPU `126.547s`，步数 `1,260.05M → 624.77M` = **`2.02×`**，峰值 `963.85 → 925.83 MiB`）。
- **独立 C04 阶段独占墙钟对照**：原生 C04 `62.608s → 9.210s`（**`6.80×`**），VM C04 `336.173s → 73.469s`（**`4.58×`**），VM C04 步数 `934.62M → ~299.34M`（**`3.12×`**）。
- **正确性与自举固定点**：`capability_c04_tests.py`（`--unbounded`）`206/206`（`3,670` 检查）四路径及 C++ 类型预言机通过；`capability_dependency_tests.py` 通过；更新后 15 模块 `selfhost/` 达成 `C2 == C3` 逐字节固定点（原生 `C2` `17.185s`，VM `C3` `166.628s` / `682.34M` 步，`566b7a698e394c9f...`）。Iter 1 及被否决的 Iter 2 等完整过程见[本轮实测记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-24-c04-incremental-dataflow.md)。
- **P0 收尾与第二台实体机同步（2026-11-25）**：适配 3 项 C04 白盒单测后 9 项相关 CTest 全部通过；在第二台 Windows x64 实体机重建并更新 `huaBin/`，完整原生生成器 C++ 生成阶段由旧版 `30.157s` 降至 `6.412s`（`4.70×`），`G1 == G2` PE 与 C++ 逐字节一致，原生 `C2`（`6.620s`）与 VM `C3`（`61.855s`、`682,337,327` 步）跨实体机逐字节复现同一 `566b7a698e394c9f...` 产物，仓库外候选编译器冷重建 `113.680s`。[验收记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-25-p0-c04-tests-and-huabin.md)

### 0.3 下一步

<a id="native-release-route-20261114"></a>

当前总优先级见[Hua 自身工具链与发行目录](设计决策与未决事项.md#hua-owned-toolchain)。N3 的最小可搬移 SDK 已验收，包内 C2/C3 为后续真实负载归因提供完整基线。Windows N2 和 N4.2B／C 已验。C3 在 271.942 秒内重建可生成真实词法器的 Hua 生成器。198 KB 输入使用带类型的字符串切片，四轮交错比较的中位用时从 0.4075 秒降至 0.0765 秒。N4.2D 和 N4.3 两个入口已原生构建，完整 HUAB 同输入下 VM 用时 303.662 秒，原生用时 52.946 秒，产物逐字节一致。N4.4 的 A、B、C 构建总用时分别为 477.641、269.703 和 300.703 秒，三代 C++ 与 B、C 的归档分别一致。[验收记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-21-n4-full-entries.md)[N4.2B／C 验收](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-21-n4-lexer.md)

#### 原生发布路线 · N2 到 N5

沿用 2026-11-14 已确认的路线。发布后端通过带类型 AST 生成 C++，使用随发行包提供的 Clang／lld。原生发布默认关闭计步，可选在循环回边及调用处检查预算。VM 继续承担快速开发、语义对照、带预算的沙箱及嵌入。其他平台验收暂缓，N2 优先 Windows。

| 阶段 | 实施顺序与交付 | 验收 |
|---|---|---|
| **N2 并发与异步** | Windows N2.1–N2.2 单计算线程和 N2.3 快照与控制均已验。覆盖容器、普通结构体和枚举、模块环境和已知闭包。Hua 自举支持捕获式的局部异步递归。N2.4 原生采用固定纤程归属的 M:N 多核。N2.5 Windows File 读写接 IOCP，按所有权和在途保活协议退休。 | 与 VM 对照返回值、错误、取消和超时。覆盖单次恢复、迟到完成、资源清理、等待不占用计算线程，实测多核并行。 |
| **N3 发布与部署** | N3.1／N3.2 已实现 Windows 原生 SDK、`hua build --release`、生成／编译／链接分段计时和普通应用独立分发。[验收](../.codex-sync/knowledge/bootstrap-performance/runs/2026-11-23-n3-native-sdk.md) | N3.3 整次缓存、N3.4 模块增量已验收。N3.5 在本机验证 Windows ZIP 搬移与原始 PE 复现，标量小程序达到 2 秒和 1 MiB 级目标。完整运行时的应用体积与独立实体机验收继续登记。 |
| **N4 统一带类型 IR 与原生自举** | Hua 从验证后的 C05 核心树生成 C++，已覆盖真实词法器使用的结构体、容器、控制流和 Result。按真实编译器依赖扩大后端，并逐项将可共享的类型化操作纳入 HUA-D009。生成器位于 `selfhost/`，后续由原生产物连续构建自身。 | 同源 HUAB 可复现，字节码与原生程序行为、诊断、退出码通过差分。用 Hua 编译器生成下一代原生编译器，再由下一代完成同源编译并验证产物与行为。 |
| **N5 扩展能力** | VM 嵌入 C API、稳定的扩展 C ABI、WASM 发布、HTTP／JSON 网络应用、parallel／SIMD、AI／Agent 运行时。按能力族推进。 | 各族分别确定接口、兼容和资源合同及验收输入。 |

N2 涉及的任务恢复、取消、预算和资源协议以[运行时与扩展设计](运行时与扩展设计.md)为准。每项原生能力用 `tests/native_backend_tests.py` 对照 VM 的标准输出、诊断及退出码。子集外输入须在生成 C++ 前给出明确诊断并以退出码 2 拒绝。`tests/native_selfhost_tests.py` 提供小规模双后端对照，完整自身验收另按第 2 节执行。

<a id="selfhost-performance-plan"></a>

#### 自举提速路线 · 先减少重复分析，再减少执行成本

1. **完成完整自身输入的阶段墙钟基线。** 固定源文件清单及内容、构建身份、工具链和预算。原生和 VM 分别记录解析／名称分析、类型分析、C04 能力分析、降级、指令生成、栈及产物校验、HUAB 编码、文件发布。记录每段墙钟，VM 同时采集阶段预算步数。嵌套阶段注明包含关系。C04 记录函数体访问数、摘要实际变化数、调用点展开的来源条目数、合并时检查及改变的字段数。诊断计数及 CPU 采样与发布计时分别采集。
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

<a id="asm-vm-feasibility-20261127"></a>

### 3.1 汇编重写 VM 底层可行性与替代方案评估（2026-11-27）

基于本仓实测剖析（真实编译中纯字节码分发仅占 `~7.2%`–`12%`，而 `Value` 复制/COW/析构分派、堆分配、`enforce_type` 字符串类型检查与 GC 扫描合计占 `>75%`）及现有 `--release` 原生后端对比（`fib` `12×`、整数容器 `21×`、自举编译 `7.8×`），评估结论如下：

- **预期收益**：手写汇编直接线索化分发仅在纯标量微基准上可提速 **`1.8×–3.0×`**；在真实编译与容器应用负载上端到端仅约 **`1.05×–1.20×`**，且永远无法逼近 `--release` 机器码。
- **核心技术难度与跨平台影响**：(1) 需先将 `src/vm.cpp` 的 C++20 协程帧改为无栈显式堆帧；(2) Windows SEH（`.seh_*`）、Linux DWARF（`.cfi_*`）与 macOS/iOS ARM64 Compact Unwind 异常展开表极易出错，否则 C++ `Diagnostic` 抛异常直接崩溃；(3) 紧耦合 48 字节 `Tagged` `Value` 布局导致维护脆弱；(4) 跨平台需同步维护 Win64 / SysV x64 / Apple ARM64（注意 iOS 禁用运行时 JIT 但允许静态编入的 AOT 汇编解释器，且严禁触碰 `x18`）至少 3 套汇编。
- **结论与三级推荐替代路线**：**现阶段不建议直接手写多平台汇编重写 `vm.cpp`**。优先按投入产出比执行：(1) 利用固定工具链 Clang 23 的 **`[[clang::musttail]]` 尾调用线索化分发**改造 `register_vm.cpp`（一份 C++ 代码覆盖 Windows/Linux/macOS/iOS 且自动生成正确展开表，可获汇编分发 85%–90% 收益）；(2) 实施 **`TypeId` 整数驻留**并扩大 `register_vm` 对结构体/闭包/外部调用的覆盖，直击 75% 非分发瓶颈；(3) 若远期布局完全冻结后确需极致解释器速度，采用单源小型汇编生成器（OfflineASM 思路）仅生成 ~30 条纯标量热指令。详见[深度分析原文](../.codex-sync/knowledge/bootstrap-performance/designs/2026-11-27-step5-decoupling-and-asm-vm-analysis.md)。

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
