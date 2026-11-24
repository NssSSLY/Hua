# 当前状态

## 2026-11-24 C04 能力分析算法改造（分解式短 ID 来源池 + 只读操作表 + SCC 局部不动点）

已完成 `selfhost/capabilities.hua` 与新增 `selfhost/origin_pool.hua` 的四阶段改造并落盘（未改 C++ 运行时，未提交 Git）。
- **冻结 14 模块普通无埋点双端对照**：原生 `52.256s → 16.822s`（`3.11×`，峰值 `175.38 → 128.22 MiB`）；VM `266.516s → 130.835s`（`2.04×`，总步数 `1,260.05M → 624.77M` = `2.02×`，峰值 `963.85 → 925.83 MiB`）；双端 HUAB 与基线逐字节一致（`db6dcfcc3d8f0ad1...`）。
- **独立 C04 阶段独占墙钟对照**：原生 `62.608s → 9.210s`（`6.80×`），VM `336.173s → 73.469s`（`4.58×`），C04 步数 `934.62M → ~299.34M`（`3.12×`）；非 C04 阶段（`325.4M` 步）首次超过 C04。
- **正确性与自举固定点**：`capability_c04_tests.py`（`--unbounded --compatibility --extra-cases c04_call_cases.json --subset-probe --type-oracle --execution-probe`）`206/206`（`3,670` 检查）四路径通过；`capability_dependency_tests.py` 通过；15 模块更新后 `selfhost/` 完成 `C1(Native) → C2(17.185s) → C3(VM 166.628s, 682.34M 步)`，`C2 == C3` 逐字节一致（`566b7a698e394c9f...`）。
- **已知余项**：3 项直接抽取私有 `View.origins` 的白盒测试需适配 `tests/capability_fixture_support.py`；`huaBin` 预装二进制未重装配。[完整记录](knowledge/bootstrap-performance/runs/2026-11-24-c04-incremental-dataflow.md)

## 2026-11-24 Windows 源码原生直跑

hua run --release 可从 Hua 源码直接原生构建并执行，复用 N3.5 发布缓存。官方 huaBin 与 ZIP 已更新。源码 VM 的 Hello 中位 16.875 毫秒，原生直跑首次 906.107 毫秒、热运行 190.961 毫秒。[实测](knowledge/bootstrap-performance/runs/2026-11-24-local-run.md)


## 2026-11-24 Windows N3.5 发行

Hello 和标量两模块冷构建中位 0.784／0.798 秒，EXE 约 0.46 MiB。带容器两模块中位 1.960 秒、最大 2.013 秒，EXE 为 1.84 MiB。正式 ZIP 通过本机搬移，四类工作负载完成原始 PE 字节复现。[实测](knowledge/bootstrap-performance/runs/2026-11-24-n3-5-windows-release.md)


## 2026-11-23 Windows N3.4 按模块增量发布

完整原生生成器首次编译 13 个对象，用时 385.679 秒。仅修改词法器实现后重编 `lexer.hua` 一个对象，用时 70.031 秒。修改实际 `Token` 布局并由 Parser 读取新字段时只重编 Lexer 和 Parser。[记录](knowledge/bootstrap-performance/runs/2026-11-23-n3-module-objects.md)



## 2026-11-23 Windows N3.3 已验收

Windows N3.3 已验收。相同输入整次复用 EXE，生成内容相同可复用对象，仅运行库变化可复用对象并重新链接。完整原生生成器冷构建 415.461 秒，删除输出后 0.529 秒恢复同哈希 EXE，生成、编译与链接均跳过。[记录](knowledge/bootstrap-performance/runs/2026-11-23-n3-native-cache.md)

## 2026-11-23 Windows N3.1／N3.2 已验收

原生 SDK 已装配到 `huaBin`。仓库外普通应用单 EXE、源码标准库、HUAB 旧入口及原生归档均通过。候选生成器用 284.219 秒完成正式发布构建，源码改动令探针输出由 13 变为 5。[验收](knowledge/bootstrap-performance/runs/2026-11-23-n3-native-sdk.md)


## 2026-11-21 Windows N4.2D、N4.3、N4.4 已验收

原生 archive.exe 用 52.946 秒编译完整归档入口，产物与 303.662 秒的 VM 基准逐字节一致。无源码复跑及导入、预算、发布失败对照通过。原生 A → B → C 闭环通过，三份 C++ 一致，B、C 的归档同源逐字节一致，源码修改进入原生子代。[实施记录](knowledge/bootstrap-performance/runs/2026-11-21-n4-full-entries.md)

## 2026-11-21 Windows N4.2B／N4.2C 已验

C3 用 271.942 秒构建 Hua 生成器。Hua 生成的真实词法器通过 10 组 AST、源码 VM、HUAB 和 EXE 对照，移走词法器源码后复跑通过。接续 N4.2D 补齐两个真实编译入口的剩余依赖。[记录](knowledge/bootstrap-performance/runs/2026-11-21-n4-lexer.md)

## 2026-11-20 Windows N4.2A 已验

Hua 生成器可处理带 int、bool、string 或普通结构体字段的跨模块程序，并按 C05 顺序执行初始化。C3 在 247.634 秒内重建生成器，七组新差分、四组标量回归和两组产物保护通过。下一片接容器、可写位置和循环。[N4.2A 记录](knowledge/bootstrap-performance/runs/2026-11-20-n4-records.md)

## 2026-11-20 Windows N4.0／N4.1 已验

C3 构建 Hua 原生生成器并经 Clang/lld 生成标量 EXE。四个程序三路径一致，两个拒绝场景保护既有产物。改一条 Hua 生成规则后同一 EXE 的输出由 13 变成 5。已盘点两个编译入口的真实节点与依赖，后续推进 N4.2。[N4 首阶段验收](knowledge/bootstrap-performance/runs/2026-11-20-n4-scalar.md)

## 2026-11-20 Windows N2 整体完成

本机完成 N2.1–N2.5。支持普通 struct/enum 快照、捕获和直接递归的局部 async fn、确定的 IOCP 在途取消及资源归还。7 个源程序五路径一致，108 个原生差分、13 个拒绝、5 项定向 CTest 通过。C2/C3 用时 247.777／251.964 秒，逐字节一致，各通过 14 个正例和 17 个反例。C05/C06 的独立报告生成、宿主指令对照和实际执行也已通过。[Windows N2 验收](knowledge/bootstrap-performance/runs/2026-11-20-n2-final.md)

## 2026-11-20 N2.3 自举局部闭包

普通局部函数和带捕获的 spawn 已由 Hua 编译器生成并执行。原反例四路均输出 6，C1、C2、C3 各通过 10 正例／13 反例。两次完整自举分别为 254.482／246.491 秒，C2/C3 逐字节相同。[证据](knowledge/bootstrap-performance/runs/2026-11-20-n2-selfhost-closure.md)

## 2026-11-20 N2 按需快照、完成投递和载荷

模块快照只复制已知可达槽位，阻塞池和 Windows IOCP 按请求唤醒，协作让出直接入队。原生和 Hua 自举前端已通过 Bytes、Optional 和 Result 载荷。模块和让出负载端到端下降 40.7% 及 47.7%–51.1%。C2/C3 分别 280.219／249.356 秒，产物逐字节一致。N2.3 的 Hua 自举嵌套闭包与更广名义载荷仍待。[本轮记录](knowledge/bootstrap-performance/runs/2026-11-20-n2-followup.md)

## 2026-11-20 N2 Task 收拢优化

常见可安全丢弃的容器结果收拢时跳过深复制。显式等待仍交付独立副本。四个 Task 各返回 75,000 行时，端到端 HUAB 949.29 → 616.30 ms，原生 679.85 → 342.55 ms。Task 收拢切片完成，N2.3 的跨编译器闭包和更多载荷及 N2 整体验收仍待。[记录](knowledge/bootstrap-performance/runs/2026-11-20-n2-task-join.md)

## 2026-11-18 Windows N2 多 worker 与 IOCP

已接入已知声明的原生闭包、模块快照、挂起清理和公开 Task 控制。Hua 前端已验顶层 spawn、taskgroup、std.task 和 std.file。1／2／4 原生 worker 用时 95.24／54.60／37.80 ms。128 MiB 文件下阻塞池／IOCP 为 31.35／25.88 ms。最终 C2／C3 逐字节相同，huaBin 已更新。N2.3 仍待 Hua 自举嵌套闭包与更广的原生 Task 数据类型。[证据](knowledge/bootstrap-performance/runs/2026-11-18-n2-windows-workers-iocp.md)

## 2026-11-18 本机 SDK 移植与 N2.3 容器快照

本机已重建并核对 huaBin，仓库外 SDK 及基线 C2/C3 通过。N2.3 的 Slice、定长数组、List 和 Map 参数与结果已通过三路径对照。新版 C2/C3 分别为 192.275／180.494 秒，产物逐字节相同。95 项原生差分、15 项拒绝和 5 项定向 CTest 通过，完整前端扩展语料在 300 秒上限停止。下一切片为闭包和模块环境快照。[记录](knowledge/bootstrap-performance/runs/2026-11-18-n2-container-tasks.md)

2026-11-18 用户确认现阶段以 Hua 自身扩展语法、标准库和产物生成能力为主，逐步收缩 C++ 实现职责。Windows huaBin 已按可搬移 SDK 交付，仓库外样例、缺依赖和 C2/C3 验收通过。[证据](knowledge/bootstrap-performance/runs/2026-11-18-windows-seed-sdk.md)实现、测量与 N2/N3/N4 的次序见[总规划](../docs/设计决策与未决事项.md#hua-owned-toolchain)。标量 async/await 已通过旧种子重建及两代验收。P0 已完成源码标准模块接入与完整阶段归因。C04 是后续性能工作的首要热点。[验收](knowledge/bootstrap-performance/runs/2026-11-18-selfhost-scalar-async.md)

## 2026-11-18 N2.1–N2.2 Windows 原生 Task

原生状态按任务保存，Windows 纤程复用 VM 的等待、定时和收拢协议。固定一个后台计算线程，支持顶层标量函数的 async、spawn、await、sleep、yield 和异步 main。92 项差分、15 项生成前拒绝通过。N1/N2 同步预热循环 52.237／52.161 ms，递归 9.389／9.309 ms。单线程八个 50 ms 等待从串行 510.812 ms 降至并发 77.089 ms。既有 VM 清理输出断言在并行编译下于 N1、N2 均有失败，N2 独立复验通过。N2.3 的数据快照和完整任务合同仍待。见[本轮证据](knowledge/bootstrap-performance/runs/2026-11-18-n2-single-worker.md)。

## 2026-11-18 Windows 跨语言基准

同机比较 Hua 源码 VM、HUAB、原生、AST 以及 Lua、CPython、Java、Go、C++。端到端计入 406 个样本，预热内核计入 80 个样本，校验值一致。AST 在 fib(30) 达到 60 秒限额，首次即终止并跳过后续 AST 项。原生预热递归 8.61 ms，HUAB 103.56 ms；30 万项整数容器原生 27.86 ms／10.8 MiB，HUAB 589.60 ms／58.9 MiB。数据与证据见[基准报告](knowledge/bootstrap-performance/runs/2026-11-18-cross-language-benchmarks.md)。

## 2026-11-18 语言百科基线确认

用户确认“目前已实现的就是确认的”。[语言百科](../docs/语言百科与设计核对.md)已统一 36 章、207 个条目的状态，保留已实现的具体适用范围和全部编号，候选、未实施事项、平台及测试证据分别标注。旧版原文归档。本轮为文档整理，原生发布与自举提速待办见 TODO，各能力的当前证据见百科与相关专题。

## 2026-11-18 性能文档整合

已将原生发布 N2–N5 与自举提速步骤合入[性能改造分析](../docs/VM与自举性能改造分析.md#03-下一步)。11 月 18 日 Windows 数据是最近的完整自身对照，分阶段归因、调用点精确失效和按需投影尚待执行。本轮整理文档，未产生新的编译或性能证据。下方各节保留当时的证据与状态记录。

## 2026-11-18 N1 Windows 两项完成

Windows 本地发布编译器与 VM 编译完整自身分别用时 48.969 和 308.588 秒，HUAB 与 C3 均逐字节相同。已修复动态接口转换和 mut 接收者别名，未定型的字段、索引及部分内建保持明确拒绝。87 项原生/VM 差分与 12 项拒绝检查通过。修复后完整编译器生成的 C++ 逐字节不变。其他平台暂缓。[验收记录](knowledge/bootstrap-performance/runs/2026-11-18-n1-windows-acceptance.md)。

## 当前主线（2026-11-17）：N1 本地后端

perf/vm-register-r1 的基线为 a4eec89。用户已授权实施 N1 性能优先方案。带类型调用和多值降级、标量及部分容器的闭包快照、同步算法回调和热点容器操作均已接入。File 沿用运行时控制块，随机算法由 VM 和本地后端共享。无计步产物通过 132 项标准库与 7 项核心基准的输出对照。 五个慢项的同机复测均快于 VM。 完整自身输入的本地与 VM HUAB 逐字节相同，墙钟 18.51／153.28 秒。

同步路径已有精确数值、固定数组和接口调用保活。N1 本批通过未注解高阶参数、异签名复合值及 mut、函数 Slice 的声明类型检查、绑定接口与索引接收者，以及隐式 File 捕获与 clone 检查的对照。Windows 原生差分 78 项一致，发布矩阵 132 标准库、7 核心与 23 示例通过。现有三个本地性能用例 A/B 变化约 ±1.1%。完整自举留作放行复验。任意未注解值的字段/索引及任意动态接口互转未纳入。见[本轮证据](knowledge/bootstrap-performance/runs/2026-11-17-n1-dynamic-arguments.md)。

短命闭包和同签名间接调用专项已完成。闭包负载 51.512→1.860 ms，堆环境和函数 Slice backing 均降至 0。间接调用 8.394→0.424 ms。真实逃逸保持堆保活。84 项原生差分及发布矩阵通过。见[本轮记录](knowledge/bootstrap-performance/runs/2026-11-17-n1-short-closures-and-calls.md)。

## 当前主线（2026-11-05收尾）：HUA-D038 P1整数/双曲交付

已按用户确认实现24个math接口：add/sub/mul/neg/abs/pow的checked/wrapping/saturating三族，加6个双曲函数。since22/HUAB22，26模块332函数（math53）；三路径128次进程检查/7074行oracle、119原生边界、21项定向CTest通过。边界加法/双曲组合相对手写约1.50/1.54倍。core纯循环首轮0.89x，独立9轮复核1.00x，未复现；其余约0.98–1.04x，原始数据保留。 [本轮记录](knowledge/bootstrap-performance/runs/2026-11-04-p1-math-arithmetic.md)。上一批6接口与性能记录完整保留，当前不据本批比较关闭其旧source字符串布局差异。文本/UTF-8 8接口已按用户定稿W1–W7完成（2026-11-07，since23/HUAB23，26模块340函数；[记录](knowledge/bootstrap-performance/runs/2026-11-07-p1-text-utf8.md)），已推送d835e57。用户2026-11-07要求的剩余六组（34函数，since24–29）也已完成并逐组推送，27模块374函数；P1只剩通用Reader/Writer；全量CTest 97/108，余11项属HUA-D036/D037已知类（[记录](knowledge/bootstrap-performance/runs/2026-11-07-p1-file-bulk.md)）。L0-4仍待selfhost since9–29同步。2026-11-07全量CTest 91/102、C2逐字节不变（见记录）。


## 最新阶段（2026-10-30）：S2-1试验未放行，默认接入撤回

用户授权开始第二阶段。16字节Entry（复杂Value入池）造成Map/真实编译退化；176字节Entry（仅位置入池）65项定向回归通过，但最终7轮ast_pool编译均略慢、中位0.97x，type_rules为1.07x，整体未放行。接入版A07/C04通过（182.38秒）；默认VM源码恢复97ecbc2后重建、16项核心复验及真实编译产物核对通过。Value仍144字节、默认Entry432字节。候选、54项存储检查与接入补丁单独保留，不把试验通过当提速。

详细结果以[性能日志](../docs/VM与自举性能改造分析.md#s2-1-results-20261030)和knowledge/bootstrap-performance/runs/2026-10-30-s2-1.md为准。原始证据只在tests/artifacts/s2-1-20261030；本轮未跑完整自举；用户随后已授权提交并推送，交付状态以Git与远端HEAD核验为准。下方旧“从S2-1开始”已被本次否决结论取代；下一项先细化S2-2非捕获标量类型槽，核对能否真正绕开通用Value/环境处理，不再仅按sizeof假设收益。


## 最新交接（2026-10-29）：第二优先方案已记录，尚未实施

用户要求记录方案并推送，以便另一台电脑继续。唯一实施顺序见[第二优先执行方案](../docs/VM与自举性能改造分析.md#execution-storage-priority2)：S2-1轻量执行栈/稳定位置池 → S2-2类型化局部槽 → S2-3调用帧/逃逸环境 → S2-4通用Value瘦身。下一次明确继续时首先限定S2-1，逐切片放行，不自动全局16字节Value或完整自举。本次仅文档，无新增性能/正确性测试结果。

源码基线9a29cc6，第一优先正确性成果保持；真实编译0.96–1.04x仍未达整体提速目标。新电脑从Git源码重建基线，tests/artifacts及tests/build不随Git。下方旧“下一项待用户选择”由本交接顺序取代；提交/远端交付以Git核验为准。

## 最新阶段（2026-10-29）：值复制第一优先

用户授权先实施结构体/字符串/Result的复制改造。长字符串不可变共享、叶记录字段体写时分离、独占成员move归一化、不可变Result载荷复用已接入；复杂外层/readonly能力投影/已发布字段位置保守处理，Task仍深快照，HUAB/Native ABI保持，Value仍144字节。

63项回归、完整A07/C04（192.57秒，外层600秒硬限）通过；新增2027检查，最终clear布局保留修复后16项核心复验通过。冻结HEAD 776d5f8同工具链新构建基线：结构体约1.13–1.26x，真实编译约0.96–1.04x；申请次数约-7.2%、申请字节约-15.5%，GC扫描仍约+19%。整体提速目标未放行，未重跑完整自举；本次用户已授权提交并推送，实际交付状态以Git及远端HEAD核验为准。广域套件出现30秒宿主前端准备超时后已停止，单独确认旧二进制也超过30秒，保留HUA-D036待办。

正式结果见docs/VM与自举性能改造分析.md本轮条目，过程knowledge/bootstrap-performance/runs/2026-10-29-value-snapshots.md；原始证据tests/artifacts/value-copy-20261029/仅本机。下一项待用户选择：先消除剩余GC/复杂记录复制成本或进入紧凑Value/类型槽与帧；不自动推进后端/网络或扩大语言语义。下方为此前日期记录，2026-10-26清理记录保持。

## 2026-10-30本轮接续

2026-10-30：当前 main@776d5f8 的 C07 正式自身闭环已验收。14模块/291088字节，210.844秒、1281255218步、peak_calls 52，C2 4195418字节、CRC32 3424058752；独立reader、编译器源码移除、生成行为和示例源码移走执行均通过。本轮未改生产源码/预算，未重跑C08或全量CTest，未提交推送。

详见[正式记录](knowledge/bootstrap-performance/runs/2026-10-30-c07-acceptance.md)。


更新2026-10-24。Git：main，HUA-D038切片2已提交推送；提交/推送以 `git log`、`git status` 为准。旧基线67faab1上的诊断与实验另存分支 `bo11-capability-diagnostics-20261023`（已推送，未合入）。

## 一句话

**2026-10-23主线调整**：用户决定暂不推进自举，在优先考虑性能的前提下完善标准库，与成熟语言常用方法基本对齐（HUA-D038）；待办见docs/标准库设计与实现路线.md“当前主线”。已完成L0-3基准框架（perf/stdlib、--suite stdlib）与切片1：strings新增19个、buffer文本4个（since 9，HUAB9），string恒为合法UTF-8确认为语言不变量；切片2（2026-10-24）：std.maps 6个、std.sets 6个（Set即`map[K]bool`的键集合）、list新增9个（since 10，HUAB10），D5查找不复制键实测退化已回退；标准库现为19模块/197函数。

Hua编译器已在Windows上自举并到达代际不动点。2026-10-23 P4-S1（C04摘要轮次按被调用方优先访问）使同机同输入步数-18.7%、墙钟-19.4%，产物不变；另一台机器上完整自举C2 207.8秒、C3 202.8秒，600秒余量约65%。2026-10-23起按用户决定暂停自举线，先做CPython对齐（P1–P4、调用路径与分发循环瘦身已完成；原机对CPython 3.12为2.4–9.7倍，另一台机器对3.14为3.1–9.9倍），下一步见TODO.md与docs/VM与自举性能改造分析.md第9.6节。

## 关键事实（均为本机实测）

| 项 | 数值 | 记录 |
|---|---|---|
| 完整自举（当前源码，P4-S1后） | 另一台机器：C2 207.8秒、C3 202.8秒，1,281,255,218步、peak_calls 52（原机F1源码343.5秒、15.70亿步） | docs/VM与自举性能改造分析.md 第0.2节“P4-S1” |
| 自身产物 | C2=C3逐字节相同，4,195,418字节，CRC32 3424058752，sha256 f73869fa…（P4-S1源码；F1源码为4,171,764字节、CRC32 559473999） | tests/build-selfhost-s1/summary.json（本机，不入Git） |
| 回归 | 原机72项CTest全过；另一台机器P4-S1后71/72，archives_extensions因本机产物长度使一个变异为空操作而失败（不含P4-S1同样失败，HUA-D037）；另11项为已定因的既有问题 | 性能日志“Windows完整CTest回归”“P4-S1” |
| 同机对照 | Hua比CPython 3.12慢约4–10倍，比Lua 5.4慢2.5–27倍 | perf/results/win-reference-stageB.json |
| 剖析（C04阶段） | 值复制约30%、内建约31%、GC约24%（D2前）；分发仅约3% | 性能日志第0.2节“第9节A/B阶段” |

## 本阶段已完成（2026-10-20～22）

1. compiler配置改为600秒墙钟为主、步数上限1000亿。
2. 修正三处Hua C04自检误报，完整自举首次成功、代际不动点（A07用例129→139）。
3. VM热路径：步骤1/2/3a与第9节B阶段（安全点批量、调用去协程、enforce_type快路径、标准库签名、参数向量池、租约控制块复用、内建名驻留与按调用点解析、运算符快路径、`!`+跳转融合按两步计、调用点布局缓存）。
4. D2：GC机制优化（函数引用访问器、回收工作区复用），策略与计数不变。
5. F1：`==`结构相等；C04比较改用`==`，步数-9.5%。
6. 测量工具：HUA_INTERNAL_PROFILE计数与探针--mix、perf/run_bench.py、ab_bench.py、count_bench.py、reference_bench.py（CPython/Lua）、tests/tools/native_profile/。
7. 文档：10份活文档已同步；docs/开发与运行指南.md新增“在另一台电脑接续开发”。
8. P4-S1（2026-10-23，另一台机器）：C04 `analyze` 第2轮起按调用图DFS后序（被调用方优先）访问dirty函数体；C04 134例、195输入报告除3例stats.work外逐字节相同、隔离测试、完整C1→C2→C3不动点通过。

## 在另一台电脑接续

按docs/开发与运行指南.md#resume-on-another-computer：git clone/pull → 准备.tools（llvm-mingw-20260922-ucrt-x86_64、wasmtime-v49.0.2，可复制本机.tools）→ scripts/build.ps1 → 跑74项CTest（archives_extensions为HUA-D037已知失败） → 可选按指南三条命令复现完整自举（约20分钟，C2=C3）。然后读TODO.md与性能日志0.3节开始下一项。

## 唯一下一步

**下一项（用户选择）：** HUA-D038第5步已全部完成（第五组test/log与`io.write_err`，HUAB18，2026-10-24）。P0缺口alg `unique`/`chunk`/`product`也已完成（HUAB19，2026-10-24，已推送f247bdb），P0 11组全部完成。P1 alg `partition`/`zip_with`/`windows`与list `dedup`也已完成（HUAB20，2026-10-25，未提交），P1 alg组完成。下一步由用户选择：其余P1（先写接口提案给用户审）或路线第6步“之后按需求”。不推进自举（用户2026-10-23决定）。

- 已完成（2026-10-24）：切片2（HUAB10）至os exit/平台（HUAB16，7980e6f，均已推送）；第5步第四组std.fs walk/copy（HUAB17，已提交推送）；第五组test/log与io.write_err（HUAB18，8358f22，已推送），结果见同文档“第5步第五组test/log结果”；结果见docs/标准库设计与实现路线.md“第5步第四组fs walk/copy结果”；进度总览见同文档“进度总览（2026-10-24）”。2026-10-24完整三代自举复验通过（gen2=gen3，身份不变）。
- 实施沿用切片验收：先建手写基线→实现→三路径测试→`ab_bench`（可疑项用两份同一base二进制交替复测；疑似退化先用相同源码重建一次）→reference_bench→CTest（archives_extensions为HUA-D037已知失败）→C2逐字节不变（f73869fa…，1,281,255,218步）。
- 经验：宿主“便利”接口不一定快（`CopyFileExW`比手写读写慢）；单项A/B出现双峰时，用去掉一个调用的变体检查是否布局效应（list/06去掉reverse后base与new互换快慢）；小幅一致变慢先与HEAD源码新建对比，再按文件二分（alg P0组定位到注册表项位置）；只在源码运行慢时，用HUAB运行与`main`开头多分配少量数据区分编译后堆状态的布局效应（alg P1组maps/02）；每次调用不要分配大缓冲；Windows深层路径需`\\?\`形式。

## 待用户决定

- GC计数合同：是否允许“无环对象不入环检测”（会改变std.gc.collect()返回计数）。
- Windows分配器（HUA-D035）：是否引入mimalloc等新依赖。
- 性能路线9.5节其余：非原子引用计数与线程边界、寄存器VM/JIT时机。

## 本地维护（2026-10-26）

按用户授权清理16个不再使用的旧产物目录及1个缓存目录，历史原件压缩为16个ZIP；净释放约547.82 MiB。当前tests/build、测试源码、仍被引用冻结基线与安装版Hua保留，前后哈希和轻量CLI检查通过；未重跑自举。归档仅本机、不随Git，完整范围与恢复见[清理记录](knowledge/maintenance/2026-10-26-tests-cleanup.md)。语言/标准库/性能主线与待办不变；本次未提交推送。
