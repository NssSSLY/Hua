# 当前状态

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

**下一项：HUA-D038第5步第四组——fs walk/copy的精确接口提案（先写提案给用户审，审定后才实施）。** 不推进自举（用户2026-10-23决定）。之后：test/log。

- 已完成（2026-10-24）：切片2（HUAB10）、L0-1回调（HUAB11）、math/encoding（HUAB12）、random/crypto（HUAB13）、deque/heap（HUAB14）、time（HUAB15，701f943，均已推送）；第5步第三组std.os exit/平台（HUAB16，已提交推送），结果见docs/标准库设计与实现路线.md“第5步第三组os exit/平台结果”。
- 现有std.fs（文本/二进制读写、stat/rename/replace/temp_file）与std.path合同见docs/标准库与内建接口.md“std.fs”“S1a：路径与文件状态详细合同”；walk/copy需与既有路径规则（Windows驱动器相对路径拒绝等）、File资源与Task阻塞适配一致，提案中定。
- 实施沿用切片验收：先建手写基线→实现→三路径测试→`ab_bench`（可疑项用两份同一base二进制交替复测；**疑似退化先用相同源码重建一次排除构建布局波动**）→reference_bench→CTest（archives_extensions为HUA-D037已知失败）→C2逐字节不变（f73869fa…，1,281,255,218步）。

## 待用户决定

- GC计数合同：是否允许“无环对象不入环检测”（会改变std.gc.collect()返回计数）。
- Windows分配器（HUA-D035）：是否引入mimalloc等新依赖。
- 性能路线9.5节其余：非原子引用计数与线程边界、寄存器VM/JIT时机。
