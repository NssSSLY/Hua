# 当前状态

更新2026-10-22。Git：main，最新提交见 `git log -1`（本次交接提交在15d3c79之后），已推送origin/main。

## 一句话

Hua编译器已在Windows上自举并到达代际不动点；同一时段完整自举从555.6秒（首次成功）降到约344秒，600秒余量约43%。下一步是继续性能路线（F其余项与C阶段）。

## 关键事实（均为本机实测）

| 项 | 数值 | 记录 |
|---|---|---|
| 完整自举（当前源码） | 343.5秒、1,570,130,785步、peak_calls 52 | docs/VM与自举性能改造分析.md 第0.2节“D2＋F1” |
| 自身产物 | C2=C3逐字节相同，4,171,764字节，CRC32 559473999，sha256 454c7f77…（交接前更正注释日期后重做） | tests/build-selfhost-f1/summary-datefix.json（本机，不入Git） |
| 回归 | 72项CTest全过；另11项为已定因的既有问题（宿主前端过慢造成的测试时限、3项旧断言） | 性能日志“Windows完整CTest回归” |
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

## 在另一台电脑接续

按docs/开发与运行指南.md#resume-on-another-computer：git clone/pull → 准备.tools（llvm-mingw-20260922-ucrt-x86_64、wasmtime-v49.0.2，可复制本机.tools）→ scripts/build.ps1 → 跑72项CTest → 可选按指南三条命令复现完整自举（约20分钟，C2=C3）。然后读TODO.md与性能日志0.3节开始下一项。

## 唯一下一步

docs/VM与自举性能改造分析.md 第0.3节：F其余项（std.map原生合并/包含、strings.cut_once、C04来源路径整数化）与C阶段（不可变共享字符串、16字节Value）。每个子项：ab_bench对比 → 完整自举（改编译器源码时重做C2=C3）→ 72项CTest → 更新性能日志与本文件。

## 待用户决定

- GC计数合同：是否允许“无环对象不入环检测”（会改变std.gc.collect()返回计数）。
- Windows分配器（HUA-D035）：是否引入mimalloc等新依赖。
- 性能路线9.5节其余：非原子引用计数与线程边界、寄存器VM/JIT时机。
