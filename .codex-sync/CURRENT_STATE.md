# 当前状态

更新2026-10-22。**当前源码完整直接自举成功并到达代际不动点。** HEAD 67faab1，所有改动未提交、未推送。

- 接续此前会话（Linux云环境）的VM步骤1/2：compiler配置改为600秒墙钟为主、步数上限1000亿；安全点批量、空drain跳过、调用/返回/内建同步路径。工作日志在docs/VM与自举性能改造分析.md第0节。本轮在Windows上用scripts/build.ps1同一工具链重新构建了tests/build全部目标。
- 不限步数后暴露三处Hua C04误报，均与C++检查器（src/sema.cpp）规则不一致，已在selfhost/capabilities.hua修正：Member保留record_copy、缺省投影的复制记录成员、list.snapshot元素类型；map循环绑定类型；空map字面量元素形状。tests/data/a07_completion_cases.json新增10例（129→139，含3个反例）。
- 验证：ctest capability_c04通过（AST/VM/HUAB/SB1＋oracle＋prepared）。完整自举555.6秒、1,735,764,055步，自身HUAB 4,182,995字节、CRC32 1559818211、sha256 55ec7f09…9881；gen2 542.9秒且逐字节相同；宿主与自身编译器编译type_rules.hua的产物相同。产物与脚本在tests/build-selfhost-c04fix/。
- 边界：步骤3a后600秒余量约126秒（21%），每步约273ns；C09/G3未做。
- Windows完整CTest（10-22）：83项71通过/12失败，逐项定因未发现步骤1/2或C04修复导致的回归；本轮引起的capability_instantiation（测试抽取函数列表）已修。其余为早已存在的3项断言和本机宿主前端36–39秒造成的测试时限，时限测试后段检查未执行到。
- 步骤3a（10-22）：剖析后做四项语义不变修改（enforce_type精确匹配快路径、invoke_standard签名只按首参、调用参数向量池、copy_value共享struct不重复打包）；试过的“参数移入”无收益且使struct_pass变慢，已撤回。Windows微基准1.07–1.19x，真实负载1.08/1.09x；完整自举474.3秒（原555.6），产物逐字节相同、步数不变；此前通过的72项CTest全过。win-step2.json已用步骤3前二进制重测覆盖（首次受负载影响）。新增perf/ab_bench.py、tests/tools/native_profile/。
- 文档同步（10-22）：概览、设计决策（§2/§4、HUA-D034更新，新增D035分配器/D036宿主前端慢/D037既有断言）、开发指南（完整自举复现命令，已实跑第1步）、运行时（compiler 1000亿步、VM热路径小节）、百科HUA-32.04/34.03状态段、README与文档目录已按现状更新；相对链接全部可解析。
- 过程：knowledge/bootstrap-performance/runs/2026-10-21-c04-false-positives-selfhost.md；步骤3a在docs/VM与自举性能改造分析.md第0.2节。

- 真实负载计数（10-22，scratch测量补丁，原始数据tests/artifacts/perf-mix-20261022/）：完整自举18.3亿次堆分配（每条指令1.06次，372.8GB）；Load 28.4%（其中9,308万次读长字符串即分配）、Constant 8.6%、Member 7.0%；内建8,156万次（has/unwrap/len占57%）；struct复制4,716万次；Value 144字节、Instruction 336字节。据此在docs/VM与自举性能改造分析.md第9节写出A–G分阶段路线与预期效果。

- 第9节A/B阶段（10-22/23）：A完成（HUA_INTERNAL_PROFILE指令/值种类/内建计数、探针--mix与compiler步数上限、perf/count_bench.py、perf/reference/ CPython参照与reference_bench.py；Hua比CPython 3.12慢4.3–10.4倍）。B完成6项（租约控制块复用、BuiltinName驻留、builtin_info按调用点、运算符快路径、`!`+跳转融合按两步计、调用点布局缓存）：微基准1.02–1.32x，分配/步下降；完整自举产物与步数不变，同时段520→508秒（约-2%）；指令直方图与3a完全相同；72项CTest全过。C04阶段剖析：GC约24.5%、copy约30%、内建31.5%（json_encode 7.6%），分发仅约3%。

- D2＋F1＋Lua（10-23）：Lua 5.4.7由官方源码构建到.tools（未入库），perf/reference/*.lua；Hua比Lua慢2.5–27倍、比CPython慢约4–10倍。D2只改GC机制（HeapVisitor函数引用、回收工作区复用；时间表/计数被测试固定未动），同一C1完整自举470.1→434.8秒、产物与步数不变。F1：`==`结构相等（struct/枚举/Map/Slice/List/Result/Optional/Bytes，函数等仍E4008），C04的same_flags/view/fields改为a==b；新C1步数1.570B（-9.5%）、343.5秒，新旧C1对同一源码产出相同字节，C2=C3逐字节相同（4,171,764字节，CRC32 990536577）。72项CTest全过。文档：语言规则新增结构相等、百科HUA-06.04/09.02/12.02、运行时GC与VM、开发指南Lua/perf命令。

唯一下一步：F其余项（std.map原生合并/包含、strings.cut_once、C04来源整数化）与C阶段（共享字符串、16字节Value）；见docs/VM与自举性能改造分析.md第0.3节。待用户决定：GC计数合同（是否允许无环对象不入环检测）、Windows分配器、9.5节其余。宿主前端过慢导致的测试时限另列。
