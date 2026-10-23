# 当前状态

更新2026-10-23。Git：main，在f5e277a之上加P4-S1（只改selfhost/capabilities.hua与文档）；提交/推送以 `git log`、`git status` 为准。旧基线67faab1上的诊断与实验另存分支 `bo11-capability-diagnostics-20261023`（已推送，未合入）。

## 一句话

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

按docs/开发与运行指南.md#resume-on-another-computer：git clone/pull → 准备.tools（llvm-mingw-20260922-ucrt-x86_64、wasmtime-v49.0.2，可复制本机.tools）→ scripts/build.ps1 → 跑72项CTest → 可选按指南三条命令复现完整自举（约20分钟，C2=C3）。然后读TODO.md与性能日志0.3节开始下一项。

## 唯一下一步

2026-10-23用户决定：暂停自举线，先让VM在perf/reference微基准上赶上CPython量级（每项≤2倍），再补标准库。已完成CPython对齐P1（帧复用按形状、IterNext免复制、参数槽缓存）、P2（空作用域省略）、P3（局部变量轻量位置LocalRef，均已提交b5df3a3）与P4（标量快路径，未提交）；当前Hua/CPython 2.4–9.7倍（perf/results/win-reference-parity-p4.json）。下一步调用路径（03_calls）与P5槽位化局部变量，路线见docs/VM与自举性能改造分析.md 9.6，结论记在0.2“CPython对齐阶段”。每步：ab_bench → reference_bench → 72项CTest → C1编译自身得C2逐字节相同。

## 待用户决定

- GC计数合同：是否允许“无环对象不入环检测”（会改变std.gc.collect()返回计数）。
- Windows分配器（HUA-D035）：是否引入mimalloc等新依赖。
- 性能路线9.5节其余：非原子引用计数与线程边界、寄存器VM/JIT时机。
