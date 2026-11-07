# 已确定决定与当前方案

2026-11-04用户明确确认下一批24接口按推荐实施；2026-11-05收尾。checked返回Result<int,string>，wrapping按64位补码，saturating钳制；pow指数非负、0**0=1，checked负指数为Err MATH_DOMAIN，其余为E4003；双曲函数沿用Result<float,string>及有限值。普通算术与除法规则保持。

2026-11-04用户选择标准库其余P1主线并确认首批6接口按推荐实施；64位补码，旋转次数0–63、越界E4003，Set按键存在。已完成本批功能验收，性能保留按运行记录；S2-2候选暂不展开。


## 2026-10-30 S2-1性能闸门决定

Entry尺寸下降未产生整体收益；两个接入候选撤回默认，保留实验与反例。S2-1未完成，不据此推进全局Value替换；后续S2-2先核定标量槽如何绕开通用Value处理，保持全部既定语义和超时即停规则。

## 2026-10-29 第二优先交接决定

用户要求记录并跨电脑交接[第二优先执行方案](../docs/VM与自举性能改造分析.md#execution-storage-priority2)，实施顺序固定为S2-1栈/位置池、S2-2类型槽、S2-3帧/逃逸环境、S2-4Value。保留第一优先语义成果，不承诺旧估算1.5–2倍，不以尺寸减少替代端到端收益。每切片独立验收、可回退；本轮仅文档，后续明确继续从S2-1起。

## 2026-10-30本轮接续

2026-10-30：当前 main@776d5f8 的 C07 正式自身闭环已验收。14模块/291088字节，210.844秒、1281255218步、peak_calls 52，C2 4195418字节、CRC32 3424058752；独立reader、编译器源码移除、生成行为和示例源码移走执行均通过。本轮未改生产源码/预算，未重跑C08或全量CTest，未提交推送。 保留现行1000亿步/600秒合同，不使用旧500M门槛重新阻断；C07结束不代表全部A11/C08/C09/G3或HUAB20接口覆盖完成。

详见[正式记录](knowledge/bootstrap-performance/runs/2026-10-30-c07-acceptance.md)。


更新2026-10-22。

## 不变合同

小核心/静态类型/显式转换、struct/interface/composition；注释#及嵌套#* *#，文档标记##与#** **#预留；/真除法、//向下整除、%与//一致的余数、**幂；数组尺寸0或[1-9][0-9]*，禁止[02]/[0_2]。E0、ER-DEC-01–08、RS-DEC-01–14及A07 8层/32轮/512重置、未知保守/别名/只读/写能力保持。Hua自举可依赖C++ VM/GC/OS原语，不能让C++前端代做声称由Hua完成的分析/生成。

## 2026-10-20以来的决定

- **compiler执行配置**（2026-10-20实现，用户随后要求接续该方案）：以600秒墙钟为主，步数上限1000亿只防死循环；128调用深度、GC间隔65536、16MiB报告不变；默认run配置100万步不变。
- **C04误报修复**：Hua C04与C++检查器（src/sema.cpp）规则不一致处按C++规则修正（Member保留record_copy、复制记录成员、list.snapshot元素类型、map循环绑定类型、空map元素形状）。
- **结构相等**（2026-10-22，用户授权F1）：`==`/`!=` 对struct、枚举、Map、Slice/数组、List、Result、Optional、Bytes按内容比较；函数、Task、File、Buffer、Json、Error仍E4008。规则见docs/语言设计与类型规则.md#structural-equality。C04比较函数改用`==`。
- **指令融合计步**（用户指定）：加载时融合仍按原指令条数计步；HUAB与步数不变。
- **GC**：分代时间表（7小+1全量）与回收计数由测试固定，只做机制优化；“无环对象不入环检测”会改变std.gc.collect()计数，须用户先定合同。
- **文档**：docs/VM与自举性能改造分析.md为第10份活文档（用户2026-10-22确认），记录性能步骤结论；完整逐轮过程仍在.codex-sync/knowledge。进度只在概览，任务/优先级只在设计决策。
- **性能验收方式**：每个子项用perf/ab_bench.py交替A/B（步数不变、输出与.expect一致），完整自举产物逐字节相同（改编译器源码时改为重做C2=C3），此前通过的72项CTest全过。单次计时受机器状态影响大，只比较同一时段的结果。
- **提交/推送**：仅在用户明确要求时；2026-10-22用户要求推送，已推送到origin/main。

- compiler配置600秒墙钟为主，步数上限1000亿
- ==对struct/枚举/Map/Slice/List/Result/Optional/Bytes结构相等，函数等仍E4008
- 加载时融合按原指令条数计步，HUAB与步数不变
- GC时间表与计数不变；无环对象不入环检测需先定计数合同
- docs/VM与自举性能改造分析.md为第10份活文档

- 2026-10-23 P4-S1：C04摘要第2轮起按被调用方优先访问，已在当前main上验收（同输入-18.7%步、产物不变、C2=C3）；文档按新结构更新（性能日志0.1/0.2/0.3、项目概览、开发指南、README、HUA-D037）。用户已接受stats.work一类内部计数差异。

- 2026-10-23 **用户决定调整优先级**：暂停自举（C线：C04/selfhost改造、C09/G3验收）任务，先做通用性能优化，目标是在 perf/reference 的同算法微基准上赶上CPython量级（每个基准 Hua/CPython ≤2倍，进一步争取≤1倍，以 `perf/reference_bench.py` 同机同时段实测为准）；达到后再补标准库。性能改动仍须保持语义、步数与HUAB不变，自举产物逐字节相同作为回归检查。

- 保留A与B，撤回D；激活环境shared_ptr/weak_ptr开销待寿命证明后单独设计

- 剩余逐指令开销来自协程帧、336字节Instruction与144字节Value，需P7/P6；P5收益约5–18%

- 未修改仓库代码与构建配置；方案待用户选择：A清单、B第三方分配器（暂不引入）、C从源头减少分配

- 暂停自举；标准库对齐为当前主线，VM性能作为约束与并行候选

- take_text每次验证UTF-8（跳过验证需改Buffer共享存储，未做）

- D5 transparent comparator reverted: long keys +8 percent but SSO keys -7 percent and unrelated core -4 to -6 percent; keep map_find wrapper only
- New module dispatch checks guarded by since>=10 first; unguarded string compares cost list/01 7 percent

- Routine per-thread frame pool reverted: no gain
- Callback frames point to bytecode span or own a copy; host span dangled during error unwinding

- No is_nan/is_inf (float always finite) and no math.pow (** covers)

- std.random dispatched early in invoke_standard with direct argument checks (generic checks made draws slower than a manual LCG)

- Observed about +-7 percent build-to-build layout variance on string benchmarks, non-monotonic, not attributed

- No new types: durations are int nanoseconds, time points are int Unix milliseconds

- async main body runs as a task, so os.exit there is E4104 under X3

- Replaced CopyFileExW with exclusive create plus streamed copy (CopyFileExW was slower than manual read/write)

- test/log按T1-T7推荐定稿（2026-10-24）

- alg提案按U1-U7推荐定稿（2026-10-24）

- partition多返回、zip_with带回调按较短截断、windows共享一份只读副本、dedup就地返回删除个数（用户2026-10-25定稿）

- 2026-10-29值复制使用私有表示优化，保持readonly/mut、defer、Task深快照、HUAB字符串种类4及Native ABI1；复杂外层/能力投影保守复制，不把减少申请字节当作整体提速。

- 以性能文档execution-storage-priority2为唯一详细方案；收益按交替A/B验证，超时即停，不自动全局16字节Value或完整自举。

- 不以Entry尺寸下降替代实际收益；生产VM保持原实现，候选源码归tests/experimental/s2-1，便携补丁和失败过程保留。

- 本次C07按现行1000亿步/600秒合同验收；旧500M失败只作历史。

- 用户2026-11-04已确认本批6接口推荐方案；旋转不按模64归约。

- 用户已确认6接口按推荐实施；HUAB21，旋转次数0–63、越界E4003。
- 功能验收通过；严格全项无退化保留07_strings源模式约6%差异及诊断。

- 用户确认24接口按推荐方案；since22，checked为Result、wrapping补码、saturating钳制。

- center奇数差额放右、codepoints与chars都提供、slice_runes越界Err、非法码点Err不替换、utf8_decode失败(-1,0)、HUAB23（用户2026-11-07定稿）

- 用户2026-11-07定稿六组提案全部按推荐，并要求每组提交推送；通用Reader/Writer仍后置
