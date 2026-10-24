# 待办与下一次选择

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
- P0缺口alg `unique`/`chunk`/`product`已完成（2026-10-24，HUAB19，未提交）；结果见docs/标准库设计与实现路线.md“P0缺口alg unique/chunk/product结果”；P0 11组全部完成
- 下一项（用户选择）：P1剩余项（alg `partition`/`zip`/`windows`、list `dedup`、按行缓冲读取与Reader/Writer、strings Unicode等，先写接口提案），或路线第6步按需求

- 用户选择：回补P0缺口alg unique/chunk/product（先写提案）或第6步按需求；提交推送待用户要求

- 用户选择：P1剩余项（先写提案）或第6步按需求；提交推送待用户要求
