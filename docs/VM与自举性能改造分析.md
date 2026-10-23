# VM与自举性能改造分析

本文同时是性能改造的**工作日志**：第0节记录每一步做了什么、结果如何、下一步做什么；第1–8节是最初的分析与方案（保留原文，结论被实测修正时在第0节说明）。

**文档职责（2026-10-22用户确认为docs第10份活文档）**：本文维护VM与自举性能的分析、方案和按步骤的结论性工作日志；完整逐轮过程、失败/撤回证据与运行身份在[项目记忆](../.codex-sync/knowledge/bootstrap-performance/INDEX.md)，当前能力与总状态在[项目概览](项目概览与进度.md)，项目级优先级在[设计决策](设计决策与未决事项.md)。

最初分析更新：2026-10-20。依据：项目文档、`.codex-sync`最新直接自举记录（2026-10-20-direct-selfhost-trace）、以及对 `src/vm.cpp`、`src/value.cpp`、`src/runtime.cpp`、`src/tasks.cpp`、`include/hua/value.hpp`、`include/hua/vm.hpp`、`selfhost/capabilities.hua` 的代码阅读。本文是外部审阅意见，**没有修改任何源码，也没有新的实测**；所有“预计”数字都是估算，需要用下文的基准验证。

## 0. 当前进度与下一步（工作日志）

### 0.1 当前状态（随每一步更新）

| 项 | 状态 |
|---|---|
| 当前步骤 | 2026-10-23起按用户决定暂停自举线，做CPython对齐（见0.2“CPython对齐阶段”与9.6）；P1–P4已完成，Hua/CPython 2.4–9.7倍（06_map_merge未动） |
| 下一步 | 见0.3 |
| 自举 | Windows（2026-10-23，P4-S1源码，另一台机器）：C2 207.8秒、C3 202.8秒、12.81亿步，C2=C3逐字节相同；600秒余量约65%。各次计时的机器与时段见0.2对应步骤，不跨机比较 |
| compiler配置 | 已改为墙钟600秒为主，步数上限提高到1000亿（只防死循环），见0.2 步骤1 |
| 微基准 | `perf/` 已建立；Windows结果 `perf/results/win-step2.json`、`win-step3a.json`；`perf/ab_bench.py` 交替A/B对比 |
| 已同步到本地 | 步骤1、2的代码、`perf/`、本文档（2026-10-21）；2026-10-22已在Windows上重新构建全部目标 |

### 0.2 已完成步骤

#### 步骤1（2026-10-20）：compiler配置改为墙钟为主，建立微基准与基线

**compiler配置**
- `include/hua/execution.hpp`：新增 `compiler_step_limit = 100000000000`（1000亿），`--execution-profile compiler` 使用它；default配置的100万步、128层调用、GC间隔不变。
- `selfhost/contract.json`：`compiler_budget.execution_steps` 改为 100000000000；`supervisor_seconds` 仍为600，由 `tools/hua_frontend.py` 按此墙钟终止进程。
- 同步修改断言旧值的8个测试：`tests/selfhost_{budget,pipeline_budget,archive_budget,archive,type,bytecode,lowering,scale}_tests.py`。
- `tests/tools/` 下的历史诊断脚本、各 `*_probe.cpp` 里写死的5亿是旧实验的记录参数，没有改。

**微基准（`perf/`）**

| 文件 | 测什么 |
|---|---|
| `perf/bench/01_loop.hua` | 纯分发：while循环加一 |
| `perf/bench/02_arith.hua` | 范围for＋整数运算 |
| `perf/bench/03_calls.hua` | 递归fib(27)：调用/返回 |
| `perf/bench/04_struct_pass.hua` | 含字符串/切片/嵌套struct/map的记录按值传入与返回 |
| `perf/bench/05_map_ops.hua` | map[string]bool 插入/查询/遍历 |
| `perf/bench/06_map_merge.hua` | 复刻C04 `same_flags`/`merge_view` 写法 |
| `perf/bench/07_strings.hua` | 路径字符串拼接/split（C04来源路径写法） |
| `08_real_type_rules` / `09_real_ast_pool` | 真实负载：自举编译器（HUAB）完整编译 `selfhost/type_rules.hua`、`selfhost/ast_pool.hua` |

每个基准都有 `.expect`（期望stdout；真实负载的期望是含CRC32和字节数的编译回执），输出不一致即判为语义回归。真实负载清单在 `perf/workloads.json`。

运行方式（Windows）：

```powershell
cmake --build tests/build --target hua
python perf/run_bench.py --hua tests/build/hua.exe --save perf/results/win-baseline.json   # 首次会用被测hua构建 perf/results/compiler.huab，约2分钟
python perf/run_bench.py --hua tests/build/hua.exe --compare perf/results/win-baseline.json # 之后每次改动对比
```

`--only 03` 只跑名字含03的基准；`--no-real` 跳过真实负载；`--repeat` 默认3次取中位数。

**基线（Linux云环境，clang 18 Release，2 vCPU；只用于前后对比，绝对值不等于Windows）**

| 基准 | 步数 | 墙钟(s) | ns/步 |
|---|---:|---:|---:|
| 01_loop | 33,000,014 | 1.829 | 55.4 |
| 02_arith | 26,000,016 | 1.466 | 56.4 |
| 03_calls | 7,309,647 | 0.651 | 89.1 |
| 04_struct_pass | 11,000,054 | 2.585 | 235.0 |
| 05_map_ops | 13,930,211 | 1.702 | 122.2 |
| 06_map_merge | 4,715,929 | 0.790 | 167.6 |
| 07_strings | 7,350,016 | 1.276 | 173.6 |
| 08_real_type_rules | 6,499,608 | 1.860 | 286.2 |
| 09_real_ast_pool | 35,414,673 | 9.267 | 261.7 |

**这组数据修正了最初分析的两点：**
1. 最简单的指令也要约55ns/步（正常解释器5–10ns），而真实编译负载约260–290ns/步，与Windows完整自举测得的≈290ns/步一致。**慢的大头不在分发循环本身，而在struct复制、map、字符串、内建调用这些“重”指令**（04/06/07是01的3–4倍）。所以P1第1–3项（分发循环）预计只能带来有限收益，P1第4项（类型检查去字符串）和P2（COW、map）更关键。
2. 一个空 `while` 循环每轮要11条字节码（Load/Constant/Binary/JumpFalse/EnterScope/LocateName/Constant/Store/Pop/LeaveScope/Jump）。指令数本身偏多，循环体每轮进出作用域也有成本。

**另一个发现：宿主C++前端检查自举编译器源码要约2分钟。** `hua check selfhost/archive_main.hua` 在Linux上120秒（Windows文档里的“实验宿主准备120秒”是同一件事）。这不影响自举本身（自举跑的是HUAB），但让每次“从源码构建编译器”的开发循环很慢。记为待查项（sema.cpp），不在当前主线。

#### 步骤2（2026-10-20/21）：P1 VM热路径第1–3项——安全点批量化、空drain跳过、调用/返回/内建去协程

**改了什么（语义不变）**
- `src/vm.cpp` dispatch：步数上限仍逐步精确判断（E4099位置不变）；取消检查与协作让出改为每256步一次（`poll_interval`），以前是每步一次 `co_await Yield{}` + `task_checkpoint`；共享的原子步数计数改为本地累加、批量刷新。
- File pump仍逐步检查，但只在确实创建过资源时才进入（`ResourceDomain::created_flag()`，`file_pump_pending`）。
- `LeaveScope`/`Unwind`：环境里没有defer时不再创建 `drain` 协程帧。
- 普通Hua函数调用：`enter_function` 改为同步函数，不再为每次调用创建协程帧；外部调用、task_start、parallel_map、可能挂起的内建仍走原协程路径。
- 返回：返回帧链上没有defer时走同步 `complete_return`，不再创建 `finish_return` 协程帧，也不再复制整个Frame。
- 内建调用：不可能挂起的内建（`builtin_may_suspend` 判定）直接同步调用 `invoke_builtin`，不再经过按值复制参数的协程 `invoke_resumable`。
- `Yield` 增加权重参数，保持“每1024个安全点tick让出一次”的原有节奏。

**结果（Linux，同一台机器，墙钟中位数）**

| 基准 | 步数（不变） | 基线(s) | 步骤2(s) | 加速 |
|---|---:|---:|---:|---:|
| 01_loop | 33,000,014 | 1.829 | 1.181 | 1.55x |
| 02_arith | 26,000,016 | 1.466 | 1.093 | 1.34x |
| 03_calls | 7,309,647 | 0.651 | 0.340 | 1.91x |
| 04_struct_pass | 11,000,054 | 2.585 | 1.716 | 1.51x |
| 05_map_ops | 13,930,211 | 1.702 | 1.058 | 1.61x |
| 06_map_merge | 4,715,929 | 0.790 | 0.578 | 1.37x |
| 07_strings | 7,350,016 | 1.276 | 0.852 | 1.50x |
| 08_real_type_rules | 6,499,608 | 1.860 | 1.471 | 1.27x |
| 09_real_ast_pool | 35,414,673 | 9.267 | 7.738 | 1.20x |

所有基准输出、步数与基线一致；真实负载生成的HUAB回执（CRC/字节数）一致。

**回归测试（Linux，完整CTest 83项）**：65项通过，没有任何基线通过的测试变为失败（比基线多出的一项 `task_resume_migration` 在基线那次运行中失败、本次通过，属于计时相关的测试）。其余18项在基线上也失败：多数是 `selfhost_*` 在Linux上超过测试自带的180秒监督，以及依赖Windows路径/工具的测试，与本改动无关。**Windows上需要你按第0.4节命令重跑一次完整CTest确认。**

**结论**：调用密集的代码约2倍，真实编译负载只有1.2–1.3倍。剖析（callgrind，type_rules负载）显示剩余时间主要在：值复制 `copy_value` 约19%（其中按值传struct参数约9%）、内建/标准库调用约20%（其中每次调用都按参数类型重建标准库签名、复制参数）、`enter_function`/`complete_return` 约23%、malloc/free约14%、`enforce_type` 字符串类型检查约6%。分发循环本身已不是主要成本。

#### P0（2026-10-21）：不限步数完整自举——C04能收敛，但编译器拒绝了自己的源码

**怎么测的**：步骤2的Linux构建（clang Release），`hua run compiler.huab --execution-profile compiler -- selfhost/archive_main.hua out.huab`，compiler配置步数上限1000亿，外部墙钟上限1小时。编译器HUAB由宿主C++前端从当前 `selfhost/archive_main.hua` 构建（3,622,439字节）。

| 项 | 结果 |
|---|---|
| 总步数 | 1,201,844,881（约12亿；旧合同5亿步的2.4倍） |
| 墙钟 | 780.8秒（Linux 2 vCPU，比600秒上限多约30%） |
| 峰值内存 | 约596MiB |
| 调用深度峰值 | 52/128 |
| 结束阶段 | **types阶段报错**：`E3003@744:102 [capabilities.hua] read-only location`，没有生成HUAB |

**这次测量回答了两个问题：**
1. **C04会收敛。** 去掉5亿步上限后，C04分析在约12亿步内跑完了全部14个模块（之前担心的“不收敛/指数爆炸”没有出现）。所以P4（C04算法改造）不再是阻塞项，降为后续优化。
2. **出现了一个之前被5亿步上限掩盖的正确性问题：Hua写的C04把编译器自己源码中合法的代码判成了“只读位置写入”。** 宿主C++语义分析接受同一份源码，所以这是Hua C04的误报。出错位置是 `capabilities.hua` 第744行 `out.root.writable=true`，其中 `out` 来自 `var out=args[0]`（`args` 是只读切片参数），赋值复制出的 `var` 本应可写。这一项必须先修好，否则VM再快也无法得到自举产物。

**时间上的含义**：12亿步×约650ns/步（Linux上步骤2构建的真实负载）≈780秒。要进入600秒，需要每步再快约1.3倍以上；后面还有lowering/字节码/编码阶段没有跑到，所以实际需要的余量更大。

#### P0修复（2026-10-21/22，Windows）：三处C04误报，自举首次成功

**改了什么**：只改 `selfhost/capabilities.hua`，每处都按宿主C++检查器（`src/sema.cpp`）的同一规则修正，修改前都先用小例确认“宿主接受、C04拒绝”。

| 自举停在 | 原因 | 修改 |
|---|---|---|
| `744:102 read-only location` | Member表达式清掉了 `record_copy`（C++只对Index清）；`field_fact` 缺省投影没对“复制记录的记录成员”做 `copy_view`；`list.snapshot` 签名结果为空串，丢了元素类型 | 只在Index清；缺省投影补同一规则；snapshot结果按只读 `[]T` 播种 |
| `1033:33 read-only location` | `for k, v in m` 两个绑定都用了值类型，键被当成只读记录 | 绑定类型与C++一致（键/索引在前，单绑定遍历map为键） |
| `666:13 callee writes through a read-only member/view` | 空map字面量没有元素路径事实，第一次mut调用的写效果在实例化时被保守标为只读，第二次调用被拒 | 空map字面量生成默认元素形状（同C++ `fields()`） |

后两处在原始C04上同样复现，原先被更早的错误或5亿步上限掩盖。

**验证**：`tests/data/a07_completion_cases.json` 新增10例（含3个反例，确认共享成员/循环值绑定仍判只读）；`ctest -R ^capability_c04$`（AST/VM/HUAB/SB1＋类型oracle＋prepared执行）通过。

| 完整自举（Windows，同一构建） | 结果 | 步数 | 墙钟 |
|---|---|---:|---:|
| 修复第1处后 | 停在1033:33 | 1,298,882,609 | 379.0秒 |
| 修复第1、2处后 | 停在666:13 | 1,500,506,817 | 466.3秒 |
| 三处全部修复 | **status ok**，4,182,995字节HUAB | 1,735,764,055 | **555.6秒** |
| gen2：自身产物再编译自身 | ok，与gen1**逐字节相同** | 1,735,764,055 | 542.9秒 |

另外，宿主构建的编译器与自身产物分别编译 `selfhost/type_rules.hua`，两份HUAB逐字节相同。过程记录：[.codex-sync/knowledge/…/2026-10-21-c04-false-positives-selfhost.md](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-21-c04-false-positives-selfhost.md)。

**含义**：自举已不被阻塞，但600秒只剩约44秒余量；每步约320ns（Windows，含全部阶段）。下面的性能工作现在的目标是拉开余量。

**测试工具注意**：改了selfhost源码后要先删 `tests/build/a07-c04/analyzer.huab`，否则C04测试的HUAB/SB1模式复用旧分析器，报“analyzer reports differ”。

#### Windows完整CTest回归（2026-10-22）

同一构建、默认参数顺序执行83项：**71通过、12失败**（36.7分钟）。没有基线二进制可整体对照（步骤2前的hua.exe已被重新构建覆盖），因此逐项定因；对照对象是 `tests/artifacts/build-bo11-body-20261019-r3/hua.exe`（10-19冻结运行时）和改动前的 `capabilities.hua`（scratch副本）。

| 失败项 | 原因 | 与步骤1/2、C04修复的关系 |
|---|---|---|
| capability_instantiation | 测试从 `capabilities.hua` 抽取固定函数列表组成独立程序；`field_fact` 新调用了 `record`/`copy_view` | **由C04修复引起，已修**：抽取列表加入这两个函数及 `type_rules` 导入，复测通过 |
| selfhost_ast_pool | `clone` 81层嵌套期望E4007，实得E4099（AST/VM、hua/probe四路一致） | 早已存在：10-16、10-19二进制同为E4099，10-14及更早才是E4007 |
| selfhost_frontend_extended | 不限时跑完后断言失败：`compiler_main.hua` 在C02-P2扩展解析中报E8201@14:11（期望E8202） | 早已存在：10-19二进制输出逐字相同；测试10-11、该行10-15改动 |
| selfhost_lowering | 放宽单命令时限后跑到断言：`callable_arrays` 的 `capability_passes`=4，校验上限为函数数+2=3 | 早已存在：原始C04源码和10-19二进制同为4 |
| compiler_entry、selfhost_types、selfhost_bytecode、selfhost_archive、type_contract、array_size_syntax | 宿主C++前端 `check/build` 自举入口要36–39秒，超过测试固定的单命令30秒；放宽到60秒后，array_size_syntax通过，其余转为耗尽900秒套件时限（compiler_entry写死30秒） | 早已存在：新/旧二进制、新/旧C04源码的check时间相同（36–39秒）；宿主前端不经过VM |
| selfhost_parser | 超过CTest 180秒；不限时跑完**通过**（88例/922检查，404秒） | 单次调用耗时与10-19二进制相同（interpret约1.5秒） |
| capability_provenance | 超过600秒；每例4条分析路径，每次约29秒 | 新二进制/改后源码28.9秒、改前源码30.6秒、10-19二进制28.6秒，相同 |

**结论**：没有发现步骤1/2或C04修复让原本能通过的测试失败；唯一由本轮引起的 capability_instantiation 已修。未能确认的部分：selfhost_types/bytecode/archive、type_contract、capability_provenance 在测试自带的60秒/900秒上限内跑不完，后段检查本轮没有执行到。这些时限是本机宿主前端速度问题，与VM改动无关，记为待办（测试时限或宿主sema速度，见第0.2步骤1“宿主前端约2分钟”）。

#### 步骤3a（2026-10-22，Windows）：先剖析，再做四项语义不变的热路径修改

**Windows基线与测量方法**
- 第一次保存的 `win-step2.json` 受后台负载影响（如03_calls 1.516秒，同一二进制安静时0.442秒），已用步骤3前的同一二进制重测覆盖。单次 `run_bench.py` 结果会漂移5–10%，改动前后对比改用新增的 `perf/ab_bench.py`：同一基准内两个构建交替运行取中位数。
- 新增Windows原生剖析工具 `tests/tools/native_profile/`（见tests/目录说明.md）。原有主线程采样器在本机只得到ntdll帧，新变体每次采样最忙线程。

**剖析结论（真实负载：自举编译器编译ast_pool.hua，4096样本）**：内建调用32%（其中标准库23.5%、list等11%），`copy_value` 约19–32%，`operator new` 13%，GC回收约9%，`enter_function` 13%；自身时间约26%在系统模块（ntdll/ucrtbase，主要是Windows堆分配/释放）。`invoke_standard` 每次都给全部参数生成类型字符串并重建签名、参数向量逐个扩容；`enforce_type` 对精确匹配也要解析类型字符串。

**改了什么（语义不变）**
- `enforce_type`（src/value.cpp）：值与期望类型精确一致时直接返回——bool/int/string、有限的float、名字相同的struct/PlainRecord；这些情况原通用路径也是原样返回（float非有限值仍走原路径报错）。
- `invoke_standard`（src/stdlib.cpp）：签名只在alg/simd/task/list模块依赖实参类型，且只依赖首参类型（simd另看个数）；其余模块直接用声明，不再为每个参数生成类型字符串；参数向量预留容量。
- VM调用（src/vm.cpp、include/hua/vm.hpp）：调用参数向量改为小池借还（租约式，外部函数重入也安全），不再每次调用分配。
- `copy_value(Value&&)`：共享struct先转复制路径，不再先做一次注定失败的打包尝试（原来共享struct会尝试打包两次）。
- 试过并撤回：调用时把参数移入 `copy_value(Value&&)`。真实负载无可测收益（A/B 0.99x），04_struct_pass反而慢约7–13%。

**结果（Windows，同一机器，`run_bench.py` 5次中位数；A/B交替结果一致）**

| 基准 | 步数（不变） | 步骤2(s) | 步骤3a(s) | 加速 |
|---|---:|---:|---:|---:|
| 01_loop | 33,000,014 | 0.957 | 0.838 | 1.14x |
| 02_arith | 26,000,016 | 1.059 | 0.986 | 1.07x |
| 03_calls | 7,309,647 | 0.442 | 0.373 | 1.19x |
| 04_struct_pass | 11,000,054 | 1.765 | 1.643 | 1.07x |
| 05_map_ops | 13,930,211 | 1.045 | 0.926 | 1.13x |
| 06_map_merge | 4,715,929 | 0.581 | 0.538 | 1.08x |
| 07_strings | 7,350,016 | 0.896 | 0.767 | 1.17x |
| 08_real_type_rules | 6,502,585 | 0.973 | 0.900 | 1.08x |
| 09_real_ast_pool | 35,427,791 | 4.787 | 4.373 | 1.09x |

（Linux与Windows的绝对值不可比；08/09步数与Linux基线略有差异，因为编译器HUAB由修复C04后的源码构建。）

**完整自举**：474.3秒（步骤3前555.6秒，-14.6%），1,735,764,055步不变，产物与此前gen1逐字节相同（sha256 55ec7f09…9881）。

**回归**：此前通过的72项CTest（含已修的capability_instantiation）全部通过，596秒；其余11项为已定因的早已存在问题，未重跑。

#### 第9节A/B阶段（2026-10-22，Windows）：测量基础与调度/内建快路径

**A阶段（测量基础）——已完成**
- 内部计数并入 `HUA_INTERNAL_PROFILE`（普通构建零开销）：指令直方图、Load/Constant值种类、`copy_value` 按种类、内建按名计数（`include/hua/performance.hpp`、`src/vm.cpp`、`src/value.cpp`）。`hua_performance_probe`/`hua_memory_probe` 新增 `--mix`，步数上限改用compiler配置（原写死5亿），`hua_memory_probe` 在汇总行输出本机分配次数/字节。构建：`tests/build-profile`（`-DHUA_INTERNAL_PROFILE=ON -DBUILD_TESTING=OFF`）。
- `perf/count_bench.py`：每个基准的步数、分配/步、指令前四与内建前三（结果 `perf/results/win-counts-*.json`）。
- `perf/reference/0X_*.py` 与 `perf/reference_bench.py`：与微基准算法相同、输出与 `.expect` 相同的CPython 3.12参照，同机交替计时。Lua未安装（需下载许可），暂未提供。
- **同机参照结果（步骤3a构建）**：Hua比CPython 3.12慢4.3–10.4倍（01_loop 7.3x、02_arith 5.9x、03_calls 6.6x、04_struct_pass 4.3x、05_map_ops 5.4x、06_map_merge 10.4x、07_strings 6.4x），见 `perf/results/win-reference-step3a.json`。

**B阶段（调度与内建快路径）——已完成以下各项，HUAB不变、步数不变**
1. 激活租约控制块复用：每次进入作用域/调用的 `shared_ptr` 控制块改由每线程小空闲表分配（`LeaseAllocator`，线程退出后的释放直接归还系统）。01_loop分配/步0.091→0。
2. 内建名驻留：`Callable::builtin` 改为 `BuiltinName`（进程级驻留表），加载函数值不再复制字符串。
3. 内建按调用点解析：`builtin_info(name)` 一次算出核心内建编号、标准函数与可挂起分类（与 `builtin_may_suspend` 同一规则）；`invoke_builtin` 的55处名字比较改为编号比较，并省去对每个非标准内建的 `numeric_spec(name)` 解析；VM按调用点缓存（键为驻留名指针）。
4. 运算符快路径：同类型int比较、int `+ - * // %`（溢出、除零、MIN/-1仍走原路径报错）、bool与string比较、bool `!` 原地计算；其余走 `binary_value`/`unary_value`。
5. 加载时融合 `!`＋JumpFalse/JumpTrue：只在“第二步不会让出、无待处理文件pump、不超步数上限”时融合，并按两步计数；计数构建证实9个负载的指令直方图与步骤3a**完全相同**，`fused_steps` 记录融合次数（09为129,593次；多数 `!` 不直接接条件跳转，融合机会比预估少）。
6. 调用点缓存被调函数布局：每次调用的3次哈希查找（function_layout、functions.find、code_layout）改为按调用点缓存。
- 未做（留到后续）：Load＋Member、Constant＋Binary融合；Callable整体瘦身（目前只驻留了名字）；Member的 `len` 预标记（实测成本很小）。

**结果**
| 基准 | 步数（不变） | 步骤3a(s) | B阶段(s) | 加速 | 分配/步 3a→B |
|---|---:|---:|---:|---:|---|
| 01_loop | 33,000,014 | 0.838 | 0.744 | 1.13x | 0.091→0.000 |
| 02_arith | 26,000,016 | 1.069 | 0.936 | 1.14x | 0.231→0.154 |
| 03_calls | 7,309,647 | 0.427 | 0.374 | 1.14x（布局缓存另+1.06x） | 0.218→0.087 |
| 04_struct_pass | 11,000,054 | 1.907 | 1.816 | 1.05x | 1.037→0.982 |
| 05_map_ops | 13,930,211 | 1.065 | 0.809 | 1.32x | 0.121→0.075 |
| 06_map_merge | 4,715,929 | 0.637 | 0.568 | 1.12x | 0.590→0.539 |
| 07_strings | 7,350,016 | 0.860 | 0.697 | 1.23x | 0.429→0.388 |
| 08_real_type_rules | 6,502,585 | 0.989 | 0.967 | 1.02x | 0.453→0.411 |
| 09_real_ast_pool | 35,427,791 | 4.836 | 4.640 | 1.04x | 0.406→0.366 |

（`perf/ab_bench.py` 交替7次中位数；第3/6项前的数字是第1–5项累计，布局缓存单独A/B。）

**完整自举**：产物与此前逐字节相同、1,735,764,055步不变。同一时段对照：步骤3a二进制519.7秒、B阶段508.3秒（约-2%，各一次，处于机器噪声量级）；本机整体比测得474.3秒时慢，绝对时间不可与之前直接比较。**回归**：此前通过的72项CTest全部通过（685秒）。

**为什么B阶段对完整自举收益小（修正第9.2节对B的预估）**：完整自举主要时间在C04阶段，那里的剖析（前120秒，37,306个样本）为：GC回收约24.5%（`collect_records` 内的 `std::function` 访问器、`weak_ptr` lock/release、地址索引）、`copy_value` 约30%、内建约31.5%（其中 `json_encode` 7.6%，来自C04用 `json.encode(a)==json.encode(b)` 比较字段表）；`binary_value` 只有0.5%、名字查找2.3%。B阶段针对的分发/查找成本在这里本来就小。据此调整后续顺序：先做**D2（GC侵入式跟踪、无环对象不入环检测）**与**F1的聚合相等**（直接替换 `json_encode` 比较），再做C阶段。

#### D2（GC机制）＋F1（结构相等）＋Lua参照（2026-10-22，Windows）

**Lua参照**：从lua.org官方源码（5.4.7，sha256与官方公布值一致）用本仓库llvm-mingw静态构建到 `.tools/lua-5.4.7/src/lua.exe`（未入库，步骤见开发指南），`perf/reference/0X_*.lua` 输出与 `.expect` 一致。同机（B阶段构建）Hua对比：CPython 3.12慢3.9–9.6倍、Lua 5.4慢2.5–26.7倍（01_loop 23.9x、02_arith 26.7x、03_calls 13.1x、04_struct_pass 4.8x、05_map_ops 4.0x、06_map_merge 9.3x、07_strings 2.5x），`perf/results/win-reference-stageB.json`。

**完整自举里的GC实测**（计数构建，`hua_performance_probe`）：一次自举4,381次回收、累计访问存活对象4.44亿次、边4.53亿条，GC计时157秒/585秒（27%），平均每个存活对象约354ns。分代时间表（7次小回收＋1次全量）与工作量记账由 `gc_generation_tests` 固定，因此D2本轮只改机制、不改策略与结果：
1. `HeapVisitor` 由 `std::function` 改为非拥有的函数引用（每条边少一次类型擦除调用）。
2. 每次回收的地址索引、边表、入边计数、标记位与工作栈改为堆内复用（`ManagedHeap::Scratch`），不再每次分配/释放数MB；固定的候选对象在任何退出路径都释放。`gc_allocation_contract` 改为“逐个注入分配失败直到回收完成”，验证的性质（失败不丢根、之后可恢复）不变。
- 未做：不追踪无环对象（会改变 `std.gc.collect()` 的返回计数，属于合同变化）；小/全量回收时间表调整（被测试固定）。

**F1结构相等**：`==`/`!=` 对struct（含打包记录）、枚举、Map、Slice/数组、List、Result、Optional、Bytes按内容递归比较（`src/value.cpp`，AST与VM共用；规则见[语言规则](语言设计与类型规则.md#structural-equality)）。两个检查器原本就允许同类型 `==`，以前运行时报E4008；函数、Task、File、Buffer、Json、Error仍报E4008。新增 `tests/runtime/236_aggregate_equality.hua`、`237_function_equality_unsupported.hua` 与 `binary_collection_tests.py` 的List/Bytes/枚举CLI用例。C04的 `same_flags`/`same_view`/`same_fields` 改为一行 `a==b`（替代逐项循环与 `json.encode` 序列化比较）。

**完整自举（同一时段、同一机器，各一次）**

| 运行 | 编译器（C1） | 步数 | 墙钟 | 产物 |
|---|---|---:|---:|---|
| B阶段二进制 | 旧C1（修改前源码构建） | 1,734,695,450 | 470.1秒 | sha 3e6f9297… |
| D2＋F1二进制 | 旧C1 | 1,734,695,450 | 434.8秒（**-7.5%，D2**） | 与上行逐字节相同 |
| D2＋F1二进制 | 新C1（F1源码构建） | 1,570,130,785（**-9.5%**） | 343.5秒（**合计-27%**） | 与上两行逐字节相同 |
| D2＋F1二进制 | C2编译自身得C3 | — | 355.7秒 | **C2=C3逐字节相同** |

（被编译的是F1后的源码，所以产物与此前的55ec7f09不同：4,171,764字节、CRC32 990536577。新旧C1对同一源码产出相同字节，说明C04改写不改变编译结果。）

**交接前复核（同日）**：`capabilities.hua` 一处注释日期由误写的10-23更正为10-22。HUAB记录源码身份，注释改动也会改变产物字节，因此在更正后的源码上重做：宿主构建新C1 → C2（341.1秒）→ C3（369.1秒），步数仍为1,570,130,785，**C2=C3逐字节相同**（4,171,764字节、CRC32 559473999、sha256 454c7f77…，记录在本机 `tests/build-selfhost-f1/summary-datefix.json`）。

**回归**：此前通过的72项CTest全部通过（614秒，含 `capability_c04` 对新C04源码的四路对照、运行时fixture与GC合同测试）。`perf/results/compiler.huab`（本地，不入库）已删除，下次 `run_bench.py` 会用当前源码重建。

#### P4-S1（2026-10-23，Windows另一台机器）：C04摘要轮次按被调用方优先访问

**改动**：`selfhost/capabilities.hua` 新增 `callee_first`：把探测期记录的 `s.dependents`（被调用方→调用方）反转成调用边，用显式栈做DFS后序（不受128层调用限制），根与边都按函数键序遍历，结果确定。`analyze` 第1轮仍按函数键序（此时才记录调用边），从第2轮起每轮开头重算顺序并按它访问dirty函数体。

**为何语义不变**：32轮上限、未收敛保守回退、最终验证循环（仍按键序，首错选择不变）都不变，只改变各轮内的访问次序。原因：`functions` 是map，原来按函数名字母序遍历，helper的新摘要要等下一轮才传到调用方；递归簇（statement/expression/expression_body/analyze）在后几轮每轮只多几十到一百多条来源，却每次完整重算约7400万步的函数体。

**同输入A/B（同一机器、同一时段、各一次）**：两份C1都由宿主从源码构建，再编译同一份未改动的 `selfhost/archive_main.hua`。

| C1 | 步数 | 墙钟 | CPU | 产物 |
|---|---:|---:|---:|---|
| 改动前源码 | 1,570,130,785 | 228.6秒 | 227.5秒 | 4,171,764字节、CRC32 559473999（与0.2上一步记录相同） |
| P4-S1源码 | 1,276,390,981（**-18.7%**） | 184.3秒（**-19.4%**） | 183.0秒 | 与上行逐字节相同 |

**完整自举（P4-S1源码）**：宿主构建C1（19.2秒，3,642,190字节）→ C2 207.8秒 → C3 202.8秒，均为1,281,255,218步、peak_calls 52；**C2=C3逐字节相同**（4,195,418字节、62,599条指令、CRC32 3424058752、sha256 f73869fa…，记录在本机 `tests/build-selfhost-s1/summary.json`）。比A/B中的12.76亿步多约490万步，是因为被编译的源码本身多了 `callee_first`。

**正确性**：`capability_c04_tests.py` 134例/713检查通过；新旧分析器对195个C04/A07输入的报告中192个逐字节相同，3个只有内部计数 `stats.work` 变小（960→899、831→808、333→309），事实、状态、错误与 `capability_passes` 全部相同（用户已接受该类差异）；实例化（4例）与元数据隔离（3例/279断言）通过。另一份基于旧基线的诊断对capabilities.hua能力事实逐条导出比较：17,825条中35条标量事实来源变少（新为旧的子集），所有可写性标志与首错相同，用户已接受。

**回归**：此前通过的72项CTest中71项通过（388.8秒）。`archives_extensions` 失败与本改动无关（不含P4-S1时同样失败）：其“把第16字节写为0”的变异在本机是空操作——HUAB记录源码路径，本机测试产物3,872字节，载荷长度0x0F00的低字节本来就是0，文件未变而被正常执行；归入HUA-D037测试脆弱性。

**过程与旧基线上的诊断**（逐函数体步骤、来源流、超5亿步测量、已撤回的候选A）保存在分支 `bo11-capability-diagnostics-20261023` 的 `.codex-sync/knowledge/bootstrap-performance/` 下；本机运行记录见 [P4-S1记录](../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-23-p4-s1-callee-first.md)。

#### CPython对齐阶段（2026-10-23起，Windows本机）

目标与分步见9.6。每步验收：`perf/ab_bench.py` 交替A/B（步数与 `.expect` 一致）、`perf/reference_bench.py` 同时段对照、72项CTest、宿主构建的C1编译自身得到的C2与已记录产物逐字节相同且步数不变。

**P1（帧与绑定的小开销）**：`src/vm.cpp` 退休的激活按作用域形状各留最多16个备用（替代全局64项按形状线性查找）；`IterNext` 直接按名字绑定，不再为每次迭代复制336字节的Instruction；函数参数槽位在调用点布局缓存中预先算好；分发循环内联判断待处理的文件事件。对 `hua-d2f1.exe`（D2+F1构建）A/B：01_loop 1.02x、02_arith 1.45x、03_calls 1.07x、04 1.05x、05 1.12x、06 1.10x、07 1.06x、08 1.01x、09 1.04x。`vm_slots` 的有界复用测试改为检查同一性质（每形状≤16、热形状不被冷形状挤占）。

**P2（空作用域省略）**：剖析01_loop每轮11条指令中有一对空的ENTER_SCOPE/LEAVE_SCOPE。加载时（`code_layout`）标出“作用域布局不声明任何名字、且函数体内没有Defer”的EnterScope；执行时这类作用域只记一位（帧内 `scope_depth`/`elided_scopes`，64层以内），不创建Environment；LeaveScope/Unwind仍按原数量逐层弹出逻辑作用域。若运行时仍有布局未预测到的绑定落在被省略的作用域里（例如 `vm_slots` 的dictionary_fallback），`bind_named`/BindMulti先补建该层Environment，名字可见性与生命周期与原来一致。同时把每次EnterScope/IterNext的 `std::map` 查找改为按指令下标的指针表。字节码、HUAB与步数不变。

| 基准 | P2/P1 A/B | Hua/CPython（P2，同时段） | Hua/Lua |
|---|---:|---:|---:|
| 01_loop | 1.40x | 3.90x | 16.8x |
| 02_arith | 1.01x | 3.79x | 18.9x |
| 03_calls | 1.04x | 4.84x | 12.5x |
| 04_struct_pass | 1.00x（11次复测；5次时0.89x为噪声） | 3.78x | 4.4x |
| 05_map_ops | 1.04x | 3.21x | 3.3x |
| 06_map_merge | 1.08x | 9.12x | 8.7x |
| 07_strings | 1.04x | 5.27x | 2.5x |

结果文件 `perf/results/win-parity-p2-ab.json`、`win-reference-parity-p2.json`。72项CTest：并行（-j 8）首轮 `capability_c04`、`capability_prepared_supervisor`、`bytecode_verifier_metadata` 失败，删除旧 `tests/build/a07-c04/analyzer.huab` 后单独重跑三项全部通过（属既有的产物缓存与并行负载时限问题），其余69项通过。自举回归：宿主构建C1后编译当前 `selfhost/archive_main.hua`，C2为4,195,418字节、回执CRC32 3424058752（与0.2“P4-S1”记录的C2=C3产物一致），1,281,255,218步、peak_calls 52（步数与记录相同）；本机墙钟298.3秒（与另一台机器的207.8秒不可比）。

**P3（局部变量轻量位置）**：操作数栈项增加 `LocalRef`（绑定指针＋所属Environment，24字节），`LocateName` 不再构造400字节的ValueLocation、不再复制类型字符串；`Store` 对 `LocalRef` 直接在绑定上做复合运算（整数先走 `fast_binary`）、`enforce_type` 后写回，步骤与原ValueLocation路径相同；`StoreMulti` 遇到 `LocalRef` 时转换为原ValueLocation。字段/下标位置仍走ValueLocation，但从栈上取出时改为移动而非复制。所属Environment仍随栈项保活，`vm_slots` 的“离开作用域后写入旧位置”用例行为不变。字节码、HUAB与步数不变。

| 基准 | P3/P2 A/B | Hua/CPython（P3，同时段） | Hua/Lua |
|---|---:|---:|---:|
| 01_loop | 1.18x | 3.50x | 13.7x |
| 02_arith | 1.06x | 3.52x | 17.4x |
| 03_calls | 1.05x | 4.93x | 13.7x |
| 04_struct_pass | 0.97x／0.98x（7次／11次，两次都略慢；该基准此前也测出0.89x噪声，判为持平边缘） | 3.66x | 4.5x |
| 05_map_ops | 0.94x→复测1.02x（11次） | 3.09x | 3.1x |
| 06_map_merge | 1.00x | 7.57x | 8.6x |
| 07_strings | 1.00x | 4.81x | 2.3x |
| 08_real_type_rules | 1.08x | — | — |
| 09_real_ast_pool | 1.01x | — | — |

结果文件 `perf/results/win-parity-p3-ab.json`、`win-reference-parity-p3.json`。72项CTest：并行（-j 4）首轮70项通过，`capability_c04`（177秒）与 `task_resume_public` 失败，单独重跑均通过（与P2相同的并行负载现象）。自举回归：P3构建的C1编译自身得到的C2与P2逐字节相同（4,195,418字节、回执CRC32 3424058752），1,281,255,218步不变。首次单跑墙钟343.2秒（P2单跑298.3秒）；随后用同一份C1交替计时 P2/P3/P2/P3 为341.3/321.5/300.2/295.7秒，P3每对都更快（-5.8%、-1.5%），单跑差异来自机器状态。

**P4（标量快路径）**：剖析（`tests/build-prof`，01_loop）显示分发循环自身约45%，其余是 `optional<Value>` 临时值与variant构造/析构。改动均在 `src/vm.cpp`：`int_binary_into` 把int/int的 `+ - * // % < <= > >= == !=` 结果直接写回左操作数（溢出、零除、MIN/-1等仍走原路径报错，校验通过后才写入）；Constant/Load对bool/int/float直接构造栈项；JumpFalse/JumpTrue与Pop对普通值不再经过 `pop()` 搬出（Pop仍对struct做枚举完整性检查）；`LocalRef` 上 `int op= int` 且绑定类型为 `int` 时原地计算（此时 `enforce_type`/`copy_value` 本来就是恒等）。不加新指令、不融合，HUAB与步数不变。边界脚本（负数整除/取余、`/` 真除法、比较、`MIN//1`、`+=` 溢出E4002、字符串/浮点/布尔复合赋值）P3与P4输出逐字节相同。

| 基准 | P4/P3 A/B（9次） | Hua/CPython（P4，同时段） | Hua/Lua |
|---|---:|---:|---:|
| 01_loop | 1.43x | 2.40x | 8.8x |
| 02_arith | 1.17x | 3.26x | 16.4x |
| 03_calls | 1.23x | 4.09x | 10.9x |
| 04_struct_pass | 1.03x | 3.60x | 4.1x |
| 05_map_ops | 1.07x | 2.95x | 3.0x |
| 06_map_merge | 1.01x | 9.71x | 8.3x |
| 07_strings | 1.00x | 5.21x | 2.5x |
| 08_real_type_rules | 1.01x | — | — |
| 09_real_ast_pool | 1.05x | — | — |

注意噪声：本轮一次构建失败时误把P3与自身A/B，9项差异达0.96x–1.14x，说明单项±5%以内（偶发到14%）不能当作结论，需复测。结果文件 `perf/results/win-parity-p4-ab.json`、`win-reference-parity-p4.json`。72项CTest（-j 4）全部通过。自举回归：同一份C1交替计时 P3/P4/P3/P4 为300.6/300.3/313.5/284.6秒（第二对-9.2%，第一对持平），四次均1,281,255,218步；P3、P4产生的C2与P2的sha256均为f73869fa…（与0.2“P4-S1”记录的C2=C3产物相同）。自举负载以复制、内建与GC为主，标量快路径收益有限，属预期。

### 0.3 下一步

**2026-10-23用户调整优先级**：暂停自举线（C04/selfhost改造与C09/G3验收），先做通用VM性能，目标是微基准赶上CPython量级（`perf/reference_bench.py` 同机同时段，每个基准 Hua/CPython ≤2倍，进一步≤1倍）；达到后转入补标准库。自举只作为回归检查（产物逐字节相同、步数不变）。具体分步见下方“CPython对齐阶段”记录与第9.6节。

原第0.3节内容（暂停，保留供恢复自举线时参考）：

1. 已完成：C04误报修复、Windows完整CTest回归、Windows微基准基线、步骤3a、第9节A/B阶段、D2（GC机制）、F1（结构相等）、Lua参照、P4-S1（被调用方优先调度，见上）。另列：宿主前端检查自举源码过慢导致的测试超时（HUA-D036）。
2. **建议下一步**：F其余项（`std.map` 原生合并/包含——旧基线上写过接口提案，保存在分支 `bo11-capability-diagnostics-20261023` 的 `designs/2026-10-23-s2-native-map-primitives.md`，F1已覆盖其中“相等”，需在当前代码上重估；`strings.cut_once`，C04来源整数化）与C阶段（不可变共享字符串、16字节Value）——C04阶段的复制（约30%）与长字符串分配仍是最大成本；GC若要再降需决定是否允许“无环对象不入环检测”改变 `std.gc.collect()` 计数。
3. 待决定：Windows分配器（HUA-D035）；GC计数合同（见2）；第9.5节其余合同问题。

### 0.4 在Windows上复现/验收的命令

```powershell
cmake --build tests/build -j 6
ctest --test-dir tests/build --output-on-failure            # 完整回归
python perf/run_bench.py --hua tests/build/hua.exe --save perf/results/win-step2.json
# 交替A/B：python perf/ab_bench.py --base 旧hua.exe --new tests/build/hua.exe --repeat 7
# 工作量计数（需 tests/build-profile：-DHUA_INTERNAL_PROFILE=ON -DBUILD_TESTING=OFF）：
#   python perf/count_bench.py --probe tests/build-profile/hua_memory_probe.exe --hua tests/build/hua.exe
# 同机CPython参照：python perf/reference_bench.py --hua tests/build/hua.exe
# 原生剖析：tests/tools/native_profile/（见tests/目录说明.md）
```


## 1. 结论

1. **当前失败的直接原因是5亿“步数”硬上限，不是墙钟超时。** 最近一次完整自举在约144–165秒触发E4099，600秒墙钟还远没到。VM每步变快，步数一步也不会少，所以**只优化VM，在现有合同下仍然会在同一位置失败**。
2. **但VM确实非常慢。** 5亿步/143.9秒≈348万步/秒；Parser负载1.27亿步/26.7秒≈476万步/秒，即每条字节码约210–290纳秒。成熟的switch分发栈式解释器通常每条5–30纳秒，差距约10–50倍。一旦步数问题解决，下一个撞上的就是600秒墙钟。
3. 所以总耗时是三个因子的乘积，三个都要动：

   **墙钟 = 编译器算法工作量 × 每个逻辑操作需要的字节码条数 × 每条字节码耗时**

   - 第一项是C04分析算法（函数体重访、单次成本逐轮扩大）——这是你们文档已经定位的问题。
   - 第二项是**语言/标准库缺少原生集合操作**，导致map相等、合并、集合并集等都要用Hua循环逐项写，每个元素十几步。这一项在之前的分析里基本没被单独看待。
   - 第三项是VM本身的每步开销——你判断的问题，确实存在且很严重。

## 2. VM每步开销的具体来源（代码证据）

### 2.1 每条指令都要付的固定成本（`vm.cpp` dispatch）

| 位置 | 现状 | 代价 |
|---|---|---|
| 每步 `co_await Yield{}` | 每步调用一次非内联的 `await_ready`，每1024步真正挂起一次并经过调度器（含 `Clock::now()`） | 函数调用＋计数；同步主程序也付 |
| 每步 `task_checkpoint` | 进入 `file_checkpoint` → `files->pump()`，再检查取消 | 两层函数调用 |
| 每步 `++execution_metrics->steps` | 这是 `std::atomic`（报告用 `.load()`），每步一次原子加 | 带锁指令，单线程下也付 |
| 每步 `try{}` 包住 switch | 零开销异常模型下主要影响优化器 | 妨碍寄存器分配 |
| `Entry = variant<Value, ValueLocation>` | Value本身144字节；ValueLocation内含Value、`optional<SliceValue>`、string、多个shared_ptr、一个`mutable Value scratch`，按成员估算超过500字节 | 每次压栈/弹栈搬运的是大对象 |

### 2.2 指令里携带字符串，运行时反复解析

- `Instruction` 里 `text`/`type` 都是 `std::string`。`Binary`/`Unary` 把 `op.text` 传给 `binary_value`，后者按 `op=="=="`、`op=="**"` 等**字符串比较**分支。`Store` 每次做 `op.text!="="`，复合赋值还 `substr` 出新字符串。
- **运行时类型检查用的是类型字符串。** `enforce_type` 每次调用都会 `type_arguments(expected,"Result")`（构造字符串、扫描、返回 `vector<string>`）、`numeric_spec(value_type(v))`（先把值的类型**拼成字符串**再解析）、`compatible()`（对 map/Result/multi/List/Task 五种构造器各解析一次）。Bind、每个参数、每个返回值、Store、InitField 都走这条路径。
- `Binding` 保存一份 `std::string type`；像 `capabilities.Environment` 这种超过SSO长度的类型名，每次绑定都要堆分配。
- builtin 调用按名字分发：`invoke_resumable` 是协程，**按值**接收 `std::string name`、`vector<Value> args`、`vector<bool> contextual`、`std::string inferred`（每次调用都复制），然后 `invoke_builtin` 用 `name=="task_await"`、`name=="ok"`……一串字符串比较找实现。`unwrap`、`has`、`len` 这种最高频的内建都要走完这条链。

Hua 是静态类型语言，这些检查的结果在编译期（或HUAB加载校验期）已经确定，运行时重复做字符串解析是纯浪费。

### 2.3 一次用户函数调用的分配次数

`Op::Call` → `call()` 一次调用大约包括：

1. `arguments()` 新建 `vector<Value>`；
2. `call()` 本身是协程 → 一个协程帧堆分配；
3. `activation()`：`FrameArena::acquire` 线性扫描最多64个空闲环境并 `erase`（O(64) 搬移），再用带lambda删除器的 `shared_ptr` 包装 → 控制块堆分配；
4. 每个非mut参数 `copy_value(args[i])` → 对 struct 是**整棵递归复制**（见2.4），然后 `enforce_type`、`read_only`；
5. 每个参数一个 `Binding`（类型字符串复制）；
6. `frames_.push_back` 复制 Frame（含 `vector<Iterator>`）；
7. 返回时 `finish_return` 又是一个协程帧，且 `auto f=frames_.back();` **复制整个Frame**；`copy_value` 返回值，`enforce_return` 再检查；
8. 每个 `LeaveScope` / 函数退出都 `co_await drain(env)` → 又一个协程帧，**哪怕 `deferred` 是空的**。

一次很小的Hua函数调用就有十几次堆分配，这也解释了为什么文档里GC和copy在采样中占比高（24.6% / 13.1%）。

### 2.4 struct 值语义被实现成“立即深复制”

`copy_value(StructValue)` 会新建 `StructData`（受管、登记GC），然后递归 `copy_value` 每个字段；嵌套 struct 继续递归。这个函数在 Bind、传参、返回、`unwrap`、`ArrayAppend`、`MakeMulti`、`Defer` 等处都会被调用。只读参数也复制了一份，然后再标成只读——**只读参数的复制完全没有必要**。

C04 的 `State`/`Environment`/`Value`/`View` 都是带多个 map 的 struct，在 `put`/`merge`/`invoke`/`instantiate_view_at` 之间反复按值传递。`move` 路径（use_count==1 时复用）只覆盖了一部分情况。

### 2.5 map 是红黑树＋144字节节点

`MapData::entries` 是 `std::map<variant<bool,int64,string>, Value>`。C04 几乎全是 `map[string]bool` / `map[string]View` / 嵌套 map；每个条目是一个树节点（variant key + 144字节Value），查找是 O(log n) 次字符串比较。`clone()` 一个 map 是逐节点深复制。

## 3. 为什么“步数”本身这么多：缺原生集合操作

`capabilities.hua` 里大量热点是通用集合操作，被迫用Hua循环写：

| 函数 | 本质 | 独占步数（最近完整运行） |
|---|---|---:|
| `instantiate_view_at` | 遍历 origins map、拆字符串、重写key | 42,247,683 |
| `merge_view` | `clone` + map 并集（带或运算） | 27,992,126 |
| `note_prefix` | `split` 路径 + 逐级插入前缀索引 | 24,055,442 |
| `same_flags` | 两个 `map[string]bool` 相等 | 22,507,792 |
| `same_view` | 5个标量比较 + `same_flags` | 18,217,107 |
| `storage_aliases` | 别名集合查询 | 15,397,835 |

其中 `same_flags`+`same_view`+`merge_view`+`note_prefix`+`storage_aliases` 约1.08亿步，约占C04已耗3.61亿步的30%。这些在原生实现里每次调用就是**1步**。

代码里已经有绕路的证据：`same_fields` 在 `len(a)>8` 时用 `json.encode(a)==json.encode(b)` 比较，正是因为一次原生调用只算1步，比Hua循环便宜。这说明缺的是**正当的原生能力**，而不是需要更多编码技巧。同时 `binary_value` 对聚合类型 `==` 直接报 E4008（aggregate equality is not implemented）。

## 4. 改造方案（按投入产出排序）

### P0：先拿到“真实需要多少步”这个数（约半天，只是诊断）

现在最大的未知是：去掉5亿步上限后，C04到底是**会收敛但慢**，还是**不收敛/超线性爆炸**。文档自己也写了“未证明放宽步数后仍不能完成或逻辑不收敛”。

- 加一个**仅测试用**的步数覆盖选项（例如 `--execution-step-limit`，只在测试构建或显式诊断模式中开放），用现有种子跑一次，墙钟上限放到1小时。
- 产出：总步数、C04各访问组步数、是否生成HUAB。这决定后面的重点：如果总共只要8–15亿步，P1+P3就足够通过；如果呈指数增长，就必须先改C04算法（P4）。
- 这不改生产合同，只是一次测量。

**合同建议（需要你决定）：** `compiler` 配置下的步数上限本来是防死循环的保险，但它把“编译器用了简单写法”也当成了失败。一旦P3引入原生集合操作，“一步”的含义也会变。建议 compiler 配置以墙钟（600秒）为主，步数上限提高到一个只防死循环的量级（例如100亿）或去掉；默认 run 配置保持100万步不变。

### P1：VM热路径（语义不变，预计每步耗时降到1/3–1/10）

按改动风险从低到高：

1. **批量做安全点检查。** 把 `Yield`/`task_checkpoint`/原子步数计数合并为“每N步（例如256）检查一次”：本地非原子计数器 `steps_`，到阈值时再同步到 `execution_metrics`、检查取消、检查文件pump、决定是否让出。步数上限判断仍逐步精确（一次整数比较）。E4099的触发位置不变。
2. **`drain` 只在有 defer 时进入协程。** `LeaveScope`/返回路径先判断 `env->deferred.empty()`，为空就直接退出作用域，不创建协程帧。
3. **调用/返回不经过协程帧。** 普通Hua→Hua调用本来就是在 `frames_` 上压帧，不需要 `co_await call(...)`。把 `call`/`finish_return` 改成普通函数，只有 `task_start`、`await`、async builtin、带defer的清理这些可能挂起的路径才走协程。`finish_return` 里 `auto f=frames_.back()` 改成引用/移动。
4. **指令预解码（加载时一次）。** 新增一个运行时专用的 `DecodedInstruction`：
   - 运算符变成枚举（`BinOp::Add` 等），`binary_value` 按枚举 switch；
   - builtin 名字解析为 ID 或函数指针，`Load` 一个 builtin 时直接拿到；
   - 类型字符串 intern 成 `TypeId`，并预先编译成“检查计划”：`Any`（无需检查）、`Exact(nominal id)`、`Numeric(spec)`、`Result(a,b)`、`Slice(elem)`、`Optional(inner)` 等。`enforce_type` 变成按计划的整数比较，只有真正需要转换（数值字面量上下文、固定数组）才走慢路径；
   - `Binding` 保存 `TypeId` 而不是 `std::string`。

   HUAB 格式不变：这是读入后的内存表示，字符串仍是持久形式。
5. **builtin 快速路径。** `invoke_resumable` 先判断是否为可能挂起的少数函数（task/sleep/yield/文件类在Task内），其余直接调用同步实现，参数按 `const&` 传、不复制 `name`/`args`/`contextual`/`inferred`。
6. **调用约定。** 参数直接从操作数栈窗口读取，不建 `vector<Value>`；`FrameArena` 改为按 `ScopeLayout` 分桶的空闲链表（O(1) 取还）；activation 用侵入式引用计数，避免每次分配 shared_ptr 控制块。
7. **把 Location 移出值栈。** 值栈只放 `Value`，Location 放独立的小栈（或只在 Store 系列指令间存在）。值栈元素从500+字节降到Value大小。

建议完成1–3后立刻测一次（见第5节基准），这三项改动小、收益最直接。

### P2：值表示与复制语义（预计再降1/2–1/3，同时大幅减少GC压力）

8. **struct 写时复制（COW）。** 保持语言上的值语义，但实现成：复制 = 引用计数+1；写入前若 `use_count>1` 则先复制这一层。要点：
   - `LocateName`/`LocateField`/`LocateIndex` 生成写入位置时，沿路径逐层“确保唯一”（Swift 的做法）；
   - 只读标志放在引用（StructValue）上而不是数据上，`read_only` 只改标志，不复制；
   - 只读参数传递变成 O(1)；
   - `copy_value(..., deep=true)`（`clone`）仍然是显式深复制，File 拒绝和128层限制不变。

   这项需要单独验证A07的“只读来源后代不能升级写能力”和Task快照隔离，但它是去掉第2.4节成本的正确办法，比继续扩大“use_count==1 才 move”的特判更稳。
9. **Value 瘦身。** 把 `SliceValue` 的 `element_type`(string)、`packed`、`contextual`(vector<bool>) 等不常用字段挪进堆上的 slice 头对象，Value 降到16–24字节（标签+一个指针/标量）。`NumericValue` 的 `std::string type` 改成 `TypeId`。这是改动面最大的一项，可以放在COW之后。
10. **map 改为哈希表，并让 clone 也走 COW。** `map[string]...` 用开放寻址哈希＋字符串预计算hash；`clone(map)` 变成 O(1) 共享、首次写入时复制。注意保持当前“快照遍历”和遍历顺序的合同（如果语言承诺按key有序遍历，就需要有序哈希或在遍历时排序，这点要先确认语言合同）。
11. **GC：不追踪不可能成环的对象。** 只含标量/字符串字段的 StructData、只含标量的 List/Slice 不进入循环检测集合（CPython 对 tuple/dict 做同样的“untrack”）。配合COW和更少的分配，GC 的24.6%采样占比应明显下降。

### P3：补原生集合能力（直接减少步数，预计C04步数减少约30%）

这一项同时完成“标准库未完成”的一部分：

12. **聚合相等。** `==`/`!=` 支持 map、struct、slice、List 的结构相等（原生递归比较，带深度上限）。`same_flags`/`same_view`/`same_fields` 直接变成一个表达式，`json.encode` 绕路可以删掉。
13. **`std.map`（或 `std.set`）原生操作：** `keys`、`union`/`merge_with`（带合并规则，例如 bool 取或）、`contains_all`、`equal`、`retain`/`filter_prefix`。`merge_view`、`instantiate_view_at` 的内层循环、`note_prefix` 的逐级插入都可以改写成少数原生调用。
14. **字符串路径的缓存/拆分。** `split_origin`、`depth(path)` 每次都 `strings.split` 新建数组；若保留字符串路径，至少提供 `strings.cut_once` 之类不分配数组的原生函数。

这些都是通用、可测试、可在AST/VM/HUAB三路径对照的标准库函数，不是为编译器开后门。

### P4：C04算法（编译器侧，决定能否“收敛”）

P0 的结果决定这一项的紧迫度。你们已有的 R1/R2 方向（符号化来源、精确失效）是对的，补充两点：

15. **路径和来源 intern 成整数。** 现在来源是 `"owner|path#@field"` 拼接字符串，最长512字节，作为 map key 反复拆分、拼接、比较。改为编译期一次性分配 PathId/OriginId，origins 用排序整数数组或位集；合并、相等、实例化都变成整数操作。这与P3配合效果最大。
16. **保证格高度有界。** 单次访问成本随轮次扩大（statement第3次是首次的18倍），说明事实集合在增长。给 origins 集合设上限（超过阈值就“加宽”为保守的“未知来源”），确保每个函数的摘要只能单调变化有限次。这样最坏情况下重访次数和每次成本都有界。A07的保守回退规则正好可以作为加宽的语义。
17. **摘要变化检测用版本号。** 被调用者摘要变化时递增版本，调用者只记“我看到的是哪个版本”，判断是否需要重访只比较整数，而不是做深比较。

## 5. 先建立的基准（每次改动都跑，几分钟内出结果）

现在只能靠一次跑两三分钟的Parser/完整自举来判断改进，周期太长，而且一次只测一个改动。建议先加一组微基准（Hua源码＋固定期望输出），用普通Release构建，记录 ns/步：

| 基准 | 测什么 |
|---|---|
| 空循环1亿次 `for i in 0..N {}` | 纯分发开销 |
| 整数累加循环 | Load/Binary/Store |
| 递归 fib(27) | 调用/返回开销 |
| 传入/返回一个含6个字段、2层嵌套的struct，调用100万次 | 值复制成本（COW前后对比） |
| `map[string]bool` 插入/查询/遍历10万项 | map 表示 |
| 两个1000项map的相等/合并 | P3收益 |

目标参考（同一台机器上可以顺手对照 Lua 5.4 或 CPython 3.12 的同等脚本）：空循环每步≤20ns，fib(27)≤1秒。

Parser负载（现有1.27亿步、普通CPU约26.6秒）作为真实回归：HUAB逐字节相同、步数相同（P1/P2不应改变步数），只看CPU时间。

## 6. 预期效果（估算，需基准验证）

| 阶段 | 每步耗时 | 步数 | 说明 |
|---|---|---|---|
| 当前 | 约210–290ns | 5亿步时C04未完成 | — |
| P1完成 | 约50–80ns | 不变 | 主要去掉协程帧、字符串类型检查、逐步原子/挂起检查 |
| P1+P2 | 约20–40ns | 不变 | COW去掉大部分struct复制和GC压力 |
| +P3 | 同上 | C04约减少30% | 集合操作变成单步原生调用 |
| +P4 | 同上 | 有界、可预测 | 决定是否收敛 |

如果P0测出完整自举需要10–15亿步，P1+P2之后大约30–60秒即可完成（在调整步数上限的前提下）。这是推算，不是承诺。

## 7. 不建议现在做的事

- **继续在 Hua 源码里做局部技巧优化**（例如用 json.encode 做相等比较、调整 unwrap_or 求值顺序）。每轮收益在1%左右，而且被VM开销和缺失的原生能力抵消，最近 R1/R2 就是正确性通过但步数反增的例子。
- **直接上 JIT/原生后端或寄存器VM。** 在当前的值表示（144字节variant、字符串类型、shared_ptr一切）上换执行引擎，大部分成本会原样搬过去。先把P1/P2做完，再看HUA-D028是否还需要。
- **每次只改一处就跑完整自举。** 先用第5节的微基准和Parser负载快速迭代，确认数量级提升后再跑一次完整自举。

## 8. 建议的执行顺序

1. 加微基准，记录当前 ns/步（半天）。
2. P0：测试专用步数覆盖，跑一次不限步数的完整自举，拿到真实总步数和收敛情况（半天，取决于运行时间）。
3. P1 第1–3项（安全点批量化、drain空跳过、调用去协程），测基准和Parser（1–2天）。
4. P1 第4–7项（指令预解码/TypeId、builtin快速路径、调用约定、Location分离）（3–5天）。
5. P3 聚合相等＋std.map 原生操作，改写C04热点（2–3天，可与4并行）。
6. P2 COW → map哈希化 → Value瘦身 → GC untrack（1–2周）。
7. 按P0结果决定P4的深度；调整 compiler 配置的步数合同；再跑一次完整自举并进入C08代际。

## 9. 基于真实负载剖析的优化路线（2026-10-22）

目标：把每条指令的平均耗时从现在的约273ns降到成熟解释器的量级，并减少编译器自身需要执行的指令数。本节基于两类实测（原始数据在本机 `tests/artifacts/perf-mix-20261022/`，测量补丁只在scratch副本中构建，未进入仓库源码）：

- **原生采样**（`tests/tools/native_profile/`，自举编译器编译ast_pool.hua）：内建调用32%、`copy_value` 19–32%、`operator new` 13%、GC约9%、`enter_function` 13%；约26%自身时间在ntdll/ucrtbase（Windows堆）。
- **计数**（完整自举，1,735,764,055条指令，产物与正式构建逐字节相同）。

### 9.1 实测事实

| 项目 | 数值 | 含义 |
|---|---:|---|
| 堆分配次数 | 1,833,544,648次（**每条指令1.06次**），合计372.8GB，平均203字节 | 一次474秒的自举里分配/释放与搬运字节是最大单项成本；成熟解释器的热路径基本不分配 |
| 指令构成 | Load 28.4%、Constant 8.6%、Member 7.0%、Call 6.4%、JumpFalse 4.8%、Jump 4.8%、Unary 4.3%、Binary 4.0%、CheckIndex 3.7%、LeaveScope 3.4% | 读值类指令（Load/Constant/Member）占44% |
| Load读到的值 | struct 28.2%、string 26.0%、callable 22.7%、bool 7.2%、map 5.6% | **9,308万次Load读到超过22字节的长字符串，每次都要分配一份副本**；callable每次复制128字节（含std::string） |
| Unary | `!` 7,485万次（几乎全部紧接条件跳转） | 可在加载时与跳转融合 |
| Binary | `==` 2,506万、`+` 1,906万、`!=` 1,055万 | 运算符按 `std::string` 比较分发；`+` 多为字符串拼接（分配） |
| 调用 | 用户函数3,035万次，内建8,156万次（每条指令0.064次调用） | 内建前三：`has` 30.1%、`unwrap` 17.6%、`len` 9.5%；`strings.split` 8.0%（每次新建字符串数组） |
| 复制 | struct复制4,716万次（每次新建StructData并逐字段复制）、字符串复制2,739万次、move路径字符串7,874万次 | 值语义被实现为立即复制 |
| 类型检查 | `enforce_type` 3.21亿次，步骤3a快路径命中88.3% | 剩余慢路径：`[]string` 1,196万、`map<` 1,022万、`List<` 444万、`Result<` 404万 |
| 数据结构尺寸 | Value 144字节、Instruction 336字节、ValueLocation 400字节、Callable 128字节、SliceValue 120字节、StructData 104字节 | 操作数栈元素约400字节；每条指令跨5个以上缓存行 |
| map | `std::map<variant<bool,int64,string>,Value>`；每次查找先复制键（长字符串即分配）再O(log n)字符串比较，命中后复制值 | `has` 一项就有2,452万次 |

结论：**慢的主因不是分发循环，而是“读一个值就复制、复制就分配”**。所以排序原则是先消灭热路径分配（C、D），再压缩指令与帧（B、E），并行减少编译器的步数（F）。

### 9.2 分阶段实施步骤

每个阶段都有独立验收（见9.4），一个阶段内每个子项单独提交、单独A/B，不把多项混在一次测量里。时间是单人估算。

#### A. 测量基础（约0.5周）——2026-10-22已完成，见0.2节

1. 把本次scratch计数（指令直方图、内建按名计数、Load/Constant值种类、copy_value按种类、分配次数/尺寸桶）做成正式构建选项（并入 `HUA_INTERNAL_PROFILE`，普通构建零开销），输出与 `perf/` 结果一起保存。
2. 在同一台机器上写Lua 5.4与CPython 3.12的等价微基准（01–07），得到“成熟语言”在本机的真实参照，而不是引用外部数字。
3. 每个基准增加“分配次数/指令”列；真实负载（08/09）与完整自举都记录分配次数。

效果：不直接提速；之后每一步都能回答“省掉了哪类工作”。

#### B. 调度与内建快路径（约1–1.5周，低风险，不改HUAB、不改步数）——2026-10-22已完成主要项，见0.2节；完整自举实测约-2%，低于下面的预估

1. **指令加载时预解码**：Binary/Unary的运算符转为枚举（去掉每次 `std::string` 比较）；Member预先标记 `len`；Call目标若为内建，加载时解析成内建编号与 `StandardFunction*`，并缓存“是否可能挂起”。
2. **加载时融合**（不改HUAB，执行时仍按原指令数计步）：`!`＋JumpFalse/JumpTrue→反向跳转；Load＋Member→直接取字段；Constant＋Binary比较→带常量比较。
3. **热门内建内联**：`has`、`unwrap`、`len`、`ok`、`unwrap_or`、`is_none/is_some`、`list.len/get` 走专用快路径：不建参数向量、不扫MultiValue、不经名字比较链（这7项占内建调用的70%以上）。
4. **Callable瘦身**：函数值改为指向不可变描述的指针（内建名、闭包、接收者放在共享描述里），Load一个函数不再复制128字节和字符串。
5. 每次调用的3次哈希查找（function_layout、functions.find、code_layout）改为在Callable描述或调用点缓存。

预期：每条指令-15%到-25%，完整自举约**360–410秒**。

#### C. 字符串与值表示（约3–4周，收益最大）

1. **不可变共享字符串**：`HString`（侵入式引用计数、长度、缓存的哈希；不超过15字节可内联）。复制字符串值只增减引用计数，**Load/Constant/传参/返回不再分配**；拼接与切片才分配。这一项直接消除约9,300万次长字符串Load分配和约1亿次字符串复制分配。
2. **Value压缩到16字节**：类型标签＋8字节载荷；int/float/bool/nil内联；Slice/Result/Callable/Numeric/Map等放到堆上的头对象。操作数栈、Binding、结构字段一起缩小约9倍，搬运字节随之大降。
3. **侵入式、非原子引用计数代替 `shared_ptr`**：去掉控制块分配和原子操作。值跨线程只在task启动/await、parallel_map处发生，这些边界已有 `isolated()` 复制；需逐一核对后才能用非原子计数（见9.5）。
4. **map改为哈希表**：键用 `HString`（哈希已缓存）、查找不复制键、命中不复制值（只增引用计数）。语言合同写明map“顺序无承诺”，遍历仍保持“进入for时快照”的合同。
5. 操作数栈与Location分离：栈只放16字节Value，Location在独立小栈或只在Store系列指令间存在。

预期：分配次数/指令从1.06降到约0.2–0.3；在B之后再快1.6–2.2倍，完整自举约**180–250秒**。

#### D. 结构写时复制与GC（约2–3周）——D2机制部分2026-10-22已完成（完整自举-7.5%），见0.2节；无环对象不入环检测需先定计数合同

1. **struct写时复制（COW）**：复制=引用计数+1；写入路径（LocateField/LocateIndex/Store）逐层“确保唯一”，共享时才复制这一层（Swift做法）。只读标志放在引用上，`read_only` 不再复制。4,716万次struct复制基本不再分配。
2. **GC改造**：跟踪记录改为侵入式链表（去掉每对象一个 `weak_ptr` 记录与 `std::function` 访问器）；字符串、只含标量/字符串字段的结构、标量切片不进入环检测；按“年轻代候选集”收集。
3. `clone`（显式深复制）、File拒绝、128层嵌套限制与E4099位置保持不变。

预期：再快1.3–1.5倍，完整自举约**120–190秒**；GC占比从约9%降到2%左右。

#### E. 槽位化帧与紧凑指令（约2–3周）

1. **编译期/加载期把局部变量解析为帧内槽位编号**：Load/Store变成数组下标访问；不再沿作用域链做守卫检查；EnterScope/LeaveScope在无defer、无闭包捕获时只清槽位。Environment对象只给被闭包捕获的作用域。
2. **连续调用栈**：帧放在一个连续数组里，参数直接写入被调函数的槽位，不再有激活租约 `shared_ptr` 与参数向量。
3. **类型检查计划**：加载时把类型字符串编成TypeId检查计划；字节码校验器已能证明类型相同的位置（静态类型语言中的大多数）直接省去运行时检查，其余走整数比较。
4. **紧凑指令**：热字段（操作码、2–3个整数操作数）压到16–32字节，源位置、文本、名字放到旁表，只在报错时读取。

预期：再快1.6–2.2倍，完整自举约**55–120秒**，平均每条指令约**30–70ns**，进入成熟字节码解释器常见的量级。

#### F. 减少编译器步数（约2–4周，可与C–E并行）——F1聚合相等2026-10-22已完成（步数-9.5%），见0.2节

1. **原生集合操作**（原P3）：聚合相等、`std.map` 的合并/包含/前缀筛选、`strings.cut_once`（完整自举中 `split` 调用约650万次，每次新建字符串数组）。
2. **C04来源整数化**（原P4/R1）：来源现在是 `"owner|path#@field"` 长字符串，正是长字符串Load分配的主要来源；改为编译期分配的PathId/OriginId与整数集合。
3. 有界格高度与版本号判重（原P4第16–17项）。

预期：完整自举步数减少30–50%，与C–E叠加后约**30–85秒**。这一阶段改变编译器自身的指令序列与步数，需要重新冻结身份并重做C2/C3代际比较。

#### G. 成熟解释器级（条件性，约1–2个月，E/F之后按实测决定）

1. 寄存器式VM：Load/Constant（现占37%）变成指令操作数，指令总数预计减少30–40%。
2. 线程化分发（computed goto）、NaN-boxing、Member/Call内联缓存。
3. 是否进入基线JIT/AOT（HUA-D010/D028）在此之后再定。

预期：每条（按旧指令计）约5–15ns，完整自举约**10–40秒**，达到Lua一类成熟解释器的量级。这是方向性估计，需要A阶段的本机Lua/CPython参照来校准。

### 9.3 预期效果汇总（估算，每阶段以实测为准）

| 完成到 | 完整自举（现474秒） | 平均每条指令 | 分配/指令 | 01_loop | 03_calls | 09_real |
|---|---:|---:|---:|---:|---:|---:|
| 现在（步骤3a） | 474秒 | 约273ns | 1.06 | 25ns | 51ns | 123ns |
| B（预估） | 360–410秒 | 约210–235ns | 约1.0 | ≤18ns | ≤35ns | ≤100ns |
| B（实测，2026-10-22） | 同时段508秒（步骤3a同时段520秒） | 约293ns（本时段） | 0.37（09） | 23ns | 51ns | 131ns |
| B＋D2＋F1（实测，2026-10-22） | 同时段343.5秒（B阶段同时段470.1秒） | 约219ns（15.7亿步） | — | — | — | — |
| C | 180–250秒 | 约105–145ns | 0.2–0.3 | ≤15ns | ≤30ns | ≤60ns |
| D | 120–190秒 | 约70–110ns | ≤0.1 | ≤15ns | ≤25ns | ≤45ns |
| E | 55–120秒 | 约30–70ns | ≤0.1 | ≤8ns | ≤15ns | ≤30ns |
| E＋F | 30–85秒 | 同上（步数-30–50%） | ≤0.1 | — | — | — |
| G | 10–40秒 | 约5–15ns | ≤0.05 | ≤3ns | ≤8ns | ≤15ns |

各阶段收益不能简单相乘：前一阶段去掉的成本会改变后一阶段的占比。每完成一个子项都要重测并修正本表。

### 9.4 每个子项的验收门槛（沿用步骤3a做法）

1. `perf/ab_bench.py` 交替A/B：步数不变（F阶段除外）、输出与 `.expect` 一致；任何基准退化超过3%要先解释或撤回。
2. 完整自举：产物与上一版逐字节相同（F阶段除外，F阶段改为重做C2=C3代际比较）、步数不变、记录墙钟和分配次数。
3. 此前通过的72项CTest、`capability_c04`（A07四路）全部通过；C、D阶段另加A07只读来源、Task快照隔离、GC成环回收、File拒绝与128层嵌套的专门反例。
4. 剖析复测：确认目标成本确实下降（例如C阶段的“分配/指令”），而不是只看总时间。

### 9.5 需要用户决定或确认的合同问题

| 问题 | 建议 | 影响阶段 |
|---|---|---|
| 指令融合后的步数计账 | 融合只在加载时做，按原指令条数计步（步数、E4099位置、HUAB都不变） | B、G |
| 非原子引用计数 | 值只在已有的task/parallel_map隔离边界跨线程；逐一核对后改为非原子，否则保留原子计数（收益约减三分之一） | C |
| map迭代顺序 | 语言合同已写“顺序无承诺”，改哈希表不改合同；如希望稳定顺序可用插入序哈希表（略慢） | C |
| Windows分配器（HUA-D035） | 可先引入作为临时提速（预计1.2–1.4倍），但它是新依赖；C/D完成后可再评估是否保留 | 任意 |
| 寄存器VM/JIT | 等E/F完成、有本机Lua/CPython参照后再决定 | G |

### 9.6 CPython对齐路线（2026-10-23，用户调整优先级后）

目标：`perf/reference` 同算法微基准上每项 Hua/CPython ≤2倍，再争取≤1倍；之后转入补标准库。约束不变：语义、HUAB、步数不变，自举产物逐字节相同。起点（B阶段构建）3.9–9.6倍；P2后3.2–9.1倍；P3后3.1–7.6倍（06_map_merge的同时段CPython时间波动较大）；P4后2.4–9.7倍。

剩余差距的来源（剖析01_loop/02_arith）：CPython每轮约4条字节码，Hua约9–11条，且每条要搬运144字节Value、400字节ValueLocation，局部变量按名字走Environment。因此只靠修补分发不够，必须做9.2的C/E两阶段的核心部分。顺序（每步独立验收、可单独回退）：

1. **P3 轻量位置**（2026-10-23完成）：实测01_loop 1.18x、02_arith 1.06x，低于预估的1.3–1.6倍——剩余成本在名字查找、Value复制与指令数，需P4/P5解决。
2. **P4 标量快路径**（2026-10-23完成）：实测01 1.43x、02 1.17x、03 1.23x，01_loop首次到2.4倍。下一步重点是03_calls（进入/返回函数约占46%：激活分配、参数向量、`function_layout` 哈希查找）与P5。
3. **P5 槽位化局部变量（9.2 E1）**：加载时把非捕获局部变量解析为帧内槽位，Load/Store为数组下标；Environment只给被闭包捕获或动态回退的作用域。预计循环/算术类1.5–2倍，是达到≤2倍的关键一步。
4. **P6 紧凑Value与共享字符串（9.2 C）**：Value压到16字节、字符串不可变共享；影响面最大，单独分支做，预计全部基准1.5–2倍、06/07收益最大。
5. **P7 紧凑指令（9.2 E4）**：热字段16–32字节，文本/源位置进旁表。

06_map_merge（9倍）主要成本在map红黑树与复制，归入P6与map哈希化（HUA-D004，迭代顺序合同见9.5）。每步完成后在0.2“CPython对齐阶段”追加结论行。
