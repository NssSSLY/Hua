# 待办与下一次选择

## 跨电脑交接与下一步任务优先级（2026-11-25）

- **当前远端基线（`main`）**：已完成 C04 算法改造后的全部 P0 收尾。仓内 `huaBin/` 已更新为 15 模块自举版（含 `src/compiler/origin_pool.hua`、`hua-generator.exe` `858be9fc...`、`hua-archive.exe` `140f610c...`、`compiler.huab` `b5acb320...`；15 模块 `C2 == C3` 不动点 SHA-256 为 `566b7a698e394c9f0f1b9475d74b921b97a2c8294460fafabe17dc6edf0781ec`，`682,337,327` 步，两台实体机逐字节一致）。9 项 C04 CTest 与 4 套仓库外 SDK 验收全部通过。[本轮记录](knowledge/bootstrap-performance/runs/2026-11-25-p0-c04-tests-and-huabin.md)
- **另一台电脑接手方式**：
  1. `git switch main` 并拉取最新提交后，仓库内的 `huaBin/` 可直接使用（无需先编译 C++ 即可执行 `huaBin/bin/hua.exe run`、`build --release`、`run --release`）。
  2. 如需修改或构建 C++ 宿主/运行测试探针，准备好本机 `.tools/` 后执行：`powershell -ExecutionPolicy Bypass -File .tools\build.ps1 -BuildDirectory tests/build -Parallel 8 -SkipTests`（切勿复用其他电脑的 CMake 缓存）。
- **当前主线与下一步优先级（2026-11-26 第 1 步已验收）**：
  1. **【已完成 · 第 1 步】交付第一个仓库外多包 CLI**：
     - 已生成并更新 374 项[标准库逐接口覆盖表](knowledge/bootstrap-performance/designs/2026-11-25-stdlib-coverage-matrix.md)（自举 `206/374`、`--release` `185/374`）。
     - 已实现双端 `hua.package.v1` 本地路径包解析、`@pkg/<name>/...` 统一诊断身份、首批 CLI 标准库与 `huaBin` 重装配，仓库外三包 CLI 五路输出/跨包错误位置/退出码、N3 外部包失效与 36 项负例诊断对照全部通过。[交付记录](knowledge/bootstrap-performance/runs/2026-11-26-step1-multi-package-cli.md)
  2. **【已完成 · 第 2 步】可重复构建与双端标准库单一权威合同**：
     - 已落地 `selfhost/stdlib_manifest.json`、`tests/tools/sync_stdlib_contract.py`、`stdlib_contract` CTest（`0.47s` 含 4 项负向防漏自检）及 8 模块 103 项双端打通（`SH 297/374`、`--release 288/374`、`18/27` 内建模块四路径全通，B 类纯表项清零）。[验收记录](knowledge/bootstrap-performance/runs/2026-11-26-step2-stdlib-contract.md)
     - 后续分批（按需推进）：剩余 `86` 项（`random` 句柄 5、`deque`/`random` 多态+类型 14、多态 39、高阶回调 19、`task` 原生 9）。
  3. **【已完成 · 第 3 步】SQLite 原生扩展包**：已落地同步 Native ABI v2（`Bytes`、`NativeHandle` 代次控制块与 LIFO 清理、结构化 `Result` 错误、`HUAB 30 / native_abi 2`）、`native_libs` 随包 DLL 分发、零 `wasmtime` `NativeExternalHost` 与 `packages/sqlite/`；仓库外四路径（含干净目录实跑、N3 缓存恢复 DLL、缺 DLL `E7001`）、ABI v2 6 项负向诊断与百科 61 项错误码补全全过。[验收记录](knowledge/bootstrap-performance/runs/2026-11-26-step3-sqlite-extension.md)
  4. **【已完成 · 第 4 步】流式 I/O（`std.stream`）与 HTTPS 客户端（`std.url`/`std.http` + 纯 Hua `std.sse`）**：已落地 `std.stream`（14 函数）、`std.url`（4 函数）、平台无关 `HttpBackend` + Windows 零三方 DLL `WinHttpBackend`（14 函数，严格 TLS/`ca_cert_pem`、重定向剥离凭证头、响应上限、超时与真取消）、`HUAB 31` 与纯 Hua SDK 模块 `std.sse`；4A+4B+4C（HTTPS/SSE→SQLite Agent 闭环）四路径与三路径异步取消全过。[验收记录](knowledge/bootstrap-performance/runs/2026-11-27-step4-stream-http-sse.md)
  5. **【主体完成 · 第 5 步】**第一波（5A `std.csv` + 5B `std.regex` + 5C `std.process.start`，`HUAB 32`）与第二波（5B-gzip `std.compress` + 5D 非回调多态/句柄标准库清零 + 5E `packages/python/` 封装，`HUAB 33`）均已通过 4 路径验收与 `huaBin/` 固定点重装（[第一波](knowledge/bootstrap-performance/runs/2026-11-27-step5-wave1-csv-regex-process.md) · [第二波](knowledge/bootstrap-performance/runs/2026-11-27-step5-wave2-compress-5d-python.md)）。剩余长尾：19 项高阶回调、`--release` 原生 `async/spawn`（9 项 `std.task`）、`selfhost/parser.hua` `simd` 关键字放开、Linux/iOS `HttpBackend`、三阶段 C++ 脱钩与 `HUA-D036`/`HUA-D037`。

## 当前待办 · Hua 自身工具链与发行（2026-11-18）

已完成第 1 项。Windows huaBin 在仓库外完成搬移、样例、缺依赖及 C2/C3 验收。[记录](knowledge/bootstrap-performance/runs/2026-11-18-windows-seed-sdk.md)

第 2 项标量 async/await 已通过旧种子重建与代际验收。[记录](knowledge/bootstrap-performance/runs/2026-11-18-selfhost-scalar-async.md)

P0 剩余两项已验收。[源码标准模块](knowledge/bootstrap-performance/runs/2026-11-18-sdk-source-stdlib.md) · [完整分阶段实测](knowledge/bootstrap-performance/runs/2026-11-18-full-selfhost-phase-profile.md)

Windows N2.1–N2.5 已验。当前结果包括普通结构体和枚举快照、局部 async fn 及直接递归、实际在途 IOCP 取消和最终代际检查。[Windows N2 验收](knowledge/bootstrap-performance/runs/2026-11-20-n2-final.md)

1. P1 Windows N3.5 便携 ZIP、15 模块 `huaBin` 重装配与第二台独立 Windows x64 实体机搬移及跨机字节复现已验收（[记录](knowledge/bootstrap-performance/runs/2026-11-25-p0-c04-tests-and-huabin.md)）。完整运行时应用体积优化继续登记。[N3.5 实测](knowledge/bootstrap-performance/runs/2026-11-24-n3-5-windows-release.md)
2. 并行自举提速已完成 C04 核心算法改造、3 项白盒单测适配及官方 `huaBin` 同步（[C04 实测](knowledge/bootstrap-performance/runs/2026-11-24-c04-incremental-dataflow.md) · [P0 收尾](knowledge/bootstrap-performance/runs/2026-11-25-p0-c04-tests-and-huabin.md)）。后续主选方向：(a) 剖析并提速已占 VM `52.1%` 步数的非 C04 编译阶段（Parse / Types / Lower / Verify / Archive）及清理 `HUA-D010` 枚举警告；(b) 处理 `HUA-D036`（宿主 `sema` 耗时与测试时限）和 `HUA-D037`（3 项既有断言漂移）；(c) `L0-4` 同步自举编译器 `since 9–29` 标准库签名。
3. P2 本地包及可选 C ABI／Python 适配。跨平台暂缓。完整依赖与退出条件见[总规划](../docs/设计决策与未决事项.md#hua-owned-toolchain)。

以下为按日期保存的既有任务和交接。

## 当前主线（2026-11-05收尾）：HUA-D038 P1整数/双曲交付

已按用户确认实现24个math接口：add/sub/mul/neg/abs/pow的checked/wrapping/saturating三族，加6个双曲函数。since22/HUAB22，26模块332函数（math53）；三路径128次进程检查/7074行oracle、119原生边界、21项定向CTest通过。边界加法/双曲组合相对手写约1.50/1.54倍。core纯循环首轮0.89x，独立9轮复核1.00x，未复现；其余约0.98–1.04x，原始数据保留。 [本轮记录](knowledge/bootstrap-performance/runs/2026-11-04-p1-math-arithmetic.md)。上一批6接口与性能记录完整保留，当前不据本批比较关闭其旧source字符串布局差异。下一项先给文本/编码基础（center、码点/UTF8）做精确提案，缓冲I/O另定资源合同。L0-4仍待selfhost since9–22同步；本轮没有完整自身/代际或全量CTest。两批改动均未提交推送。


## 最新阶段（2026-10-30）：S2-1试验未放行，默认接入撤回

用户授权开始第二阶段。16字节Entry（复杂Value入池）造成Map/真实编译退化；176字节Entry（仅位置入池）65项定向回归通过，但最终7轮ast_pool编译均略慢、中位0.97x，type_rules为1.07x，整体未放行。接入版A07/C04通过（182.38秒）；默认VM源码恢复97ecbc2后重建、16项核心复验及真实编译产物核对通过。Value仍144字节、默认Entry432字节。候选、54项存储检查与接入补丁单独保留，不把试验通过当提速。

详细结果以[性能日志](../docs/VM与自举性能改造分析.md#s2-1-results-20261030)和knowledge/bootstrap-performance/runs/2026-10-30-s2-1.md为准。原始证据只在tests/artifacts/s2-1-20261030；本轮未跑完整自举；用户随后已授权提交并推送，交付状态以Git与远端HEAD核验为准。下方旧“从S2-1开始”已被本次否决结论取代；下一项先细化S2-2非捕获标量类型槽，核对能否真正绕开通用Value/环境处理，不再仅按sizeof假设收益。


## 当前接续顺序（2026-10-29，取代下方旧候选顺序）

- 方案已记录，未实施：[第二优先执行方案](../docs/VM与自举性能改造分析.md#execution-storage-priority2)。
- 首项S2-1：核对基线和大型Entry成本，设计/实现轻量执行栈与稳定位置池，保持A07、defer、Task与GC寿命合同；不按每次压栈堆分配方式缩小尺寸。
- 逐切片通过后再接S2-2类型化槽、S2-3调用帧、S2-4通用Value瘦身；16字节不作先决条件。
- 交替A/B与输出/步数/产物一致；超时即停并定因。稳定退化撤回，原始日志留本机；完整自举仅阶段明确安排，600秒硬限。
- 本次授权记录/交接/推送，未执行代码改造。

## 2026-10-29 当前待选择

- 值复制第一优先切片已通过语义验收；整体性能未放行。下一候选：剩余GC扫描/复杂记录复制、紧凑Value与类型槽/调用帧，或用户选择标准库其余P1。保留固定HUA-D编号，不自动进入下一项。
- HUA-D036：大规模宿主前端构建超过测试30秒限制，已保留失败及一次对照诊断；不重试超时套件。完整自举本轮未重跑，历史197/204秒不作本轮结果。
- 下方较早“下一步”仅历史主线，最新选择以上述日期为准。

## 2026-10-30本轮接续

2026-10-30：当前 main@776d5f8 的 C07 正式自身闭环已验收。14模块/291088字节，210.844秒、1281255218步、peak_calls 52，C2 4195418字节、CRC32 3424058752；独立reader、编译器源码移除、生成行为和示例源码移走执行均通过。本轮未改生产源码/预算，未重跑C08或全量CTest，未提交推送。 C07本身不再待修；后续若继续自举线，先处理C09覆盖/兼容及HUA-D036/D037，正式C08脚本独立核定；标准库其他主线不因本次补验自动展开。

详见[正式记录](knowledge/bootstrap-performance/runs/2026-10-30-c07-acceptance.md)。


更新2026-10-23（主线改为HUA-D038标准库对齐）。完整清单与优先级以docs/设计决策与未决事项.md（design-backlog）为准；性能项的具体步骤与验收在docs/VM与自举性能改造分析.md第0.3节与第9节。

| 次序 | 待办 | 验收 |
|---|---|---|
| **当前主线** | HUA-D038 标准库对齐成熟语言（性能优先，2026-10-23用户决定；暂不推进自举）：先L0-3基准框架→字符串P0＋构建器＋parse/format→map/list P0与Set→L0-1回调入口→回调类方法→math/encoding→random/哈希→collections/time/os/fs/test（清单见docs/标准库设计与实现路线.md#stdlib-parity-2026-10-23）；L0-3基准框架已完成（perf/stdlib、--suite stdlib、strings基线），切片1（strings P0 19个＋buffer文本4个＋parse/format，since 9/HUAB9）与切片2（maps 6、sets 6、list 9，since 10/HUAB10，2026-10-24）已实现、验收并推送；**下一项为L0-1回调入口，先写接口提案给用户审**（细节见CURRENT_STATE.md“唯一下一步”） | 每切片：接口文档精确签名、三路径一致、边界测试、新增微基准与CPython/Lua对照、ab_bench无退化、HUAB since |
| **当前/最高** | CPython对齐（用户2026-10-23决定，暂停自举线）：P1–P4、调用路径（03_calls 1.15x）、分发循环瘦身（01 1.09x、02 1.04x，仅LocalRef去owner保留）已完成；P7实施前剖析显示收益约微基准5–10%、真实负载2–3%（取指在L1内；真实负载以Windows堆分配22.7%、复制、GC为主），未实施；P5实施前剖析显示其收益约5–18%（01/02），逐指令分发开销（40–50%）与Value variant成本（约17%）更大，顺序待用户决定（docs/VM与自举性能改造分析.md 9.6）；原计划P5→P6 16字节Value/共享字符串→P7紧凑指令（docs/VM与自举性能改造分析.md 9.6） | reference_bench每项Hua/CPython≤2倍（再≤1倍）；ab_bench无退化、步数不变；72项CTest；C1编译自身得C2逐字节相同 |
| 之后 | 补标准库（CPython对齐达标后） | 按docs/标准库设计与实现路线.md |
| 暂停 | 性能F其余项：std.map原生合并/包含（旧基线上的接口提案在分支bo11-capability-diagnostics-20261023，F1已覆盖“相等”，需在当前代码重估）、strings.cut_once、C04来源路径整数化（HUA-D028/D008相关）；P4-S1已完成 | 完整自举步数下降；改编译器源码后C2=C3逐字节相同；AST/VM/HUAB三路径对照；72项CTest |
| P0/高 | 性能C阶段：不可变共享字符串、16字节Value、非原子引用计数（线程边界先核对）、map哈希化（HUA-D004） | 分配/步明显下降（当前完整自举约1次/步）；ab_bench无退化；完整自举产物逐字节相同 |
| P1/高 | 性能D1：struct写时复制（A07只读来源与Task隔离反例必须保持） | struct复制分配基本消除；A07四路对照通过 |
| P1/中 | HUA-D036：宿主C++前端检查自举入口36–39秒，使9项测试超出自带时限 | 决定调测试时限或加速sema，补跑这些测试后段检查 |
| P2 | HUA-D037：3项既有断言（selfhost_ast_pool E4007/E4099、frontend_extended E8201@14:11、lowering capability_passes上限）；archives_extensions“第16字节写0”变异在部分机器上是空操作 | 逐项判断测试过期还是实现回归并修复 |
| P1 | C09覆盖/兼容、正式C08/C10脚本验收、A12/B10、G1–G3 | 按设计决策原路线 |
| 待决定 | GC计数合同（无环对象不入环检测会改变std.gc.collect()计数） | 用户确认后才做 |
| 待决定 | Windows分配器（HUA-D035）：2026-10-23剖析与对比方案已完成（Segment Heap清单实测自举-4.7%；减少分配以struct写时复制收益最大；mimalloc暂不引入），见knowledge/bootstrap-performance/designs/2026-10-23-heap-allocation-options.md | 用户选择方案后才做 |
| 待决定 | 性能路线9.5节：非原子引用计数、寄存器VM/JIT时机 | 用户确认后才做 |

已完成项与数据见CURRENT_STATE.md与性能日志第0.2节，不在此重复。提交/推送需用户明确要求。

- 性能F其余项：std.map原生合并/包含、strings.cut_once、C04来源路径整数化
- 性能C阶段：共享字符串、16字节Value、map哈希化
- HUA-D036宿主前端过慢导致的测试时限；HUA-D037三项既有断言

- P5槽位化局部变量；激活环境寿命证明或侵入式计数

- 用户决定下一项：P7紧凑指令、P6紧凑Value、或for循环作用域环境

- 用户选择HUA-D035方案；候选：A清单、C4多返回值不经堆、C3 map查找不复制键、C1 struct写时复制

- 切片2（maps 6、sets 6、list 9，HUAB10）已完成并推送（2026-10-24）；D5透明比较器实测退化已回退
- L0-1回调入口与15个回调函数已完成并推送（2026-10-24，HUAB11）；结果见docs/标准库设计与实现路线.md“L0-1回调结果”
- math P0与std.encoding已完成并推送（2026-10-24，HUAB12）；结果见docs/标准库设计与实现路线.md“第4步math/encoding结果”
- std.random与std.crypto已完成并推送（2026-10-24，HUAB13）；结果见docs/标准库设计与实现路线.md“第4步后半random/crypto结果”
- 第5步第一组std.deque与std.heap已完成并推送（2026-10-24，HUAB14）；结果见docs/标准库设计与实现路线.md“第5步第一组Deque/Heap结果”
- 第5步第二组std.time时长与UTC日期已完成并推送（2026-10-24，HUAB15）；结果见docs/标准库设计与实现路线.md“第5步第二组time结果”
- 第5步第三组std.os exit/平台已完成并推送（2026-10-24，HUAB16）；结果见docs/标准库设计与实现路线.md“第5步第三组os exit/平台结果”
- 第5步第四组std.fs walk/copy已完成并推送（2026-10-24，HUAB17）；结果见docs/标准库设计与实现路线.md“第5步第四组fs walk/copy结果”
- 第5步第五组test/log与io.write_err已完成并推送（2026-10-24，HUAB18，8358f22）；结果见docs/标准库设计与实现路线.md“第5步第五组test/log结果”
- P0缺口alg `unique`/`chunk`/`product`已完成并推送（2026-10-24，HUAB19，f247bdb）；结果见docs/标准库设计与实现路线.md“P0缺口alg unique/chunk/product结果”；P0 11组全部完成
- P1 alg `partition`/`zip_with`/`windows`与list `dedup`已完成（2026-10-25，HUAB20，未提交）；结果见docs/标准库设计与实现路线.md“P1 alg partition/zip_with/windows与list dedup结果”；P1 alg组完成
- 下一项（用户选择）：其余P1（按行缓冲读取与Reader/Writer、strings Unicode、模板/千分位格式化、Set对称差、math位运算/checked整数等，先写接口提案），或路线第6步按需求


- 用户选择下一项P1；提交推送待用户要求

- 用户选择下一项P1（按行缓冲读取与Reader/Writer、strings Unicode等，先写提案）或第6步；提交推送待用户要求

- 用户选择后再处理剩余GC/复杂记录复制成本或紧凑Value/类型槽/调用帧；整体性能尚未放行，完整自举本轮未复验。

- 下一次明确继续时实现S2-1，验证稳定位置寿命及实际栈搬运成本，再决定后续切片。

- S2-1整体未完成；下一步S2-2先核定非捕获标量槽怎样绕开通用Value/环境操作，再实施最小切片，不继续仅为Entry尺寸调参。

- C07已关闭；后续自举验收关注C09覆盖/兼容、D036/D037与正式C08脚本，未自动启动。

- 2026-11-04用户已确认0–63并实现6接口；下一项checked整数/双曲函数提案，07_strings源模式性能保留。

- 24个整数/双曲接口已完成；下一项文本/编码基础精确提案。
- L0-4仍待同步selfhost since9–22；本轮不扩展自举。

- 文本/UTF-8 8接口已完成并推送（2026-11-07，since23/HUAB23，d835e57）；结果见knowledge/bootstrap-performance/runs/2026-11-07-p1-text-utf8.md。
- 剩余六组已完成并逐组推送（2026-11-07，34函数，since24–29，27模块374函数）；见knowledge/bootstrap-performance/designs/2026-11-07-p1-remaining-six-groups-proposal.md。
- P1只剩通用Reader/Writer（RS-DEC-01，需单独设计）；流式子进程未做；POSIX子进程未验证。VM提速S2-2a（作用域复用与绑定槽）已完成未提交（2026-11-07，算术1.46x、调用1.12x、真实编译持平）；2026-11-08用户选择路线重排：先做V1结构体COW，细化设计knowledge/bootstrap-performance/designs/2026-11-08-v1-struct-cow-design.md待审定（V1-D1–D4）；审定后先V1a（框架，共享关闭）再V1b。
- L0-4仍待selfhost since9–29同步。

- 用户决定是否提交since21/22/23三批；下一项由用户选择

- P1只剩通用Reader/Writer；流式子进程未做；POSIX子进程未验证；下一项由用户选择P2、第6步或VM提速S2-2

- 补齐综合同步示例中的精确数值、固定数组和接口调用，并核对复杂闭包捕获及多重赋值写目标。

- 继续 N1 的复杂捕获、动态函数值和多重赋值写目标。

- 补齐完全未知和未注解高阶签名、异签名复合值、混合声明函数容器和剩余绑定方法。
- 核对隐式捕获中的 File 保活和 clone 合同，再完成 Windows N1 放行。

- 原生发布路线从 N2.3 完整任务合同继续。自举提速路线从完整负载分阶段归因开始。

- N2.3 原生 Task 继续扩充结构体、枚举等名义载荷并完成全范围矩阵。

- N3.5 处理小项目冷构建、发行大小和原始 PE 复现；本地扩展按既定次序接续。

- 完整运行时应用尺寸与跨实体机验收，继续 C04 性能和本地包路线。

- 后续可按应用负载继续测量本地 VM 及原生运行时效。

- 下一阶段可选：剖析并提速占 VM 52.1% 步数的非 C04 编译阶段，或清理 HUA-D036/D037 测试问题，或按需提交推送本轮改动。

- 进入第 2 步：建立单一权威公共签名与 since 版本数据，自动校验各端目录并分批补齐剩余标准库双端覆盖。

- 待用户确认第 2 步方案后开始实施，或先提交当前第 1 步及文档成果。
