# 2026-10-21/22 修复C04自检误报，首次得到自身HUAB

## 任务与身份

- 任务：接续 [docs/VM与自举性能改造分析.md](../../../../docs/VM与自举性能改造分析.md) 第0.3节第1项——修复Hua C04把编译器自身合法源码判为只读写入的问题，然后在Windows上重跑完整自举。
- 平台：Windows 11，本机 `tests/build/hua.exe`（llvm-mingw clang Release，`scripts/build.ps1` 同一工具链）。本轮先用该工具链重新构建全部目标，含此前在Linux云环境完成的步骤1/2 VM改动（构建前的hua.exe是10-20旧二进制，未包含步骤2）。
- compiler配置：步数上限1000亿（步骤1），外部墙钟上限3600秒（脚本 `tests/build-selfhost-c04fix/run.py`）。
- 输入：当前 `selfhost/archive_main.hua`（14模块）。编译器HUAB每次都由宿主C++前端从当前源码重新构建。

## 发现与修复（只改 `selfhost/capabilities.hua`）

自举先后停在三处E3003，宿主C++语义分析都接受。每一处都先用小例复现，确认宿主接受、C04拒绝，再按C++检查器（`src/sema.cpp`）的同一规则修改：

| # | 自举失败位置 | 原因 | 修改 |
|---|---|---|---|
| 1 | `744:102 read-only location`（`var out=args[0]`后写`out.root.writable`） | (a) Member表达式把`record_copy`清掉，C++只对Index清（sema.cpp:491）；(b) `field_fact`缺省投影没有对“复制记录的记录成员”做`copy_view`，`field_shape`和C++都做；(c) `std.list#snapshot`签名结果为空串，丢了元素类型，复制出的记录无法识别 | (a) 只在Index时清`record_copy`；(b) 缺省投影补同一规则；(c) snapshot结果按`[]T`播种，只读 |
| 2 | `1033:33 read-only location`（`for name, f in functions`循环内`e.visible[...]=true`） | 两个循环绑定都用了map的值类型，`name`被当成只读记录F；C++为键类型（单绑定遍历map时为键，slice双绑定首项为int） | 循环绑定类型与C++一致 |
| 3 | `666:13 callee writes through a read-only member/view`（`attach`→`note_prefix`第二次调用） | 空map字面量没有元素路径事实；第一次调用的写效果在实例化时因“根非deep_owned且缺该路径”被保守标为只读，第二次调用即被拒。C++对字面量用`fields()`生成完整形状 | 空map字面量生成默认元素形状（`field_defaults`+`prefix_index`） |

第2、3项在修改前的原始C04上同样复现（不是本轮引入）；它们原先被更早的错误或5亿步上限掩盖。第3项定位时，在被测源码副本（scratchpad，不在仓库）加了临时打印，仓库源码没有加调试代码。

## 正确性

- `tests/data/a07_completion_cases.json` 新增10例（129→139）：嵌套记录副本标量可写3例＋共享成员仍只读2例（反例）、map循环键类型3例＋值绑定只读1例（反例）、空map元素重复mut调用2例。每例先用宿主 `hua check/run` 确认期望。
- `ctest -R ^capability_c04$`（AST/VM/HUAB/SB1、类型oracle、prepared执行）：通过，187.8秒。
- 中途一次CTest计时57670秒：机器睡眠导致，日志停在prepared执行步骤；重跑通过，不是测试失败。
- 测试工具注意：`tests/build/a07-c04/analyzer.huab` 若已存在会被复用，改了selfhost源码后必须先删除，否则HUAB/SB1模式跑旧分析器，报“analyzer reports differ”。

## 完整自举结果（同一输入、同一构建）

| 运行 | 结果 | 步数 | 墙钟 |
|---|---|---:|---:|
| 修复1后 | `E3003@1033:33` types阶段 | 1,298,882,609 | 379.0秒 |
| 修复1+2后 | `E3003@666:13` types阶段 | 1,500,506,817 | 466.3秒 |
| 修复1+2+3后 | **status ok**，生成HUAB | 1,735,764,055 | **555.6秒** |

成功产物：`tests/build-selfhost-c04fix/out.huab`，4,182,995字节，format 7，62,468条指令，CRC32 1559818211，sha256 55ec7f09…9881。宿主构建的编译器HUAB 3,632,912字节，宿主构建36.2秒（不计入自举时间）。peak_calls 52。

自身产物行为抽查：用宿主构建的编译器与自身产物分别编译 `selfhost/type_rules.hua`，两份HUAB逐字节相同，回执与 `perf/bench/08_real_type_rules.expect` 一致。

代际（gen2：自身产物 `out.huab` 再编译同一源码）：status ok，542.9秒，步数同为1,735,764,055，`gen2.huab` 与 `out.huab` **逐字节相同**（sha256一致），到达不动点。记录在 `tests/build-selfhost-c04fix/gen2.json`。

## 结论与边界

- 自举不再被C04误报阻塞，Windows上首次在600秒内完成并生成自身HUAB。余量只有约44秒（7%），机器负载或源码增长就可能超限；步数17.4亿约为旧5亿合同的3.5倍。
- 每步约320ns（555.6秒/17.4亿步，含全部阶段）。下一步的性能工作（步骤3：P1其余项、P3原生集合操作）仍然必要，目的是把余量拉开，而不是解除阻塞。
- 未做：`perf/run_bench.py` Windows基线、C09/G3。

## 追加（2026-10-22）：Windows完整CTest回归

83项顺序执行71通过/12失败，36.7分钟。无步骤2前二进制可整体对照，逐项定因，对照10-19冻结二进制（tests/artifacts/build-bo11-body-20261019-r3/hua.exe）与改前capabilities.hua副本：

- capability_instantiation：本轮引起（测试抽取函数列表缺`record`/`copy_view`），已在tests/capability_instantiation_tests.py补列表与`import type_rules as rules`，复测通过。
- 早已存在的断言：selfhost_ast_pool（clone 81层期望E4007，10-16起即E4099）；selfhost_frontend_extended（compiler_main.hua C02-P2报E8201@14:11，10-19二进制相同）；selfhost_lowering（callable_arrays capability_passes=4>函数数+2，改前源码与10-19二进制相同）。
- 时限：宿主C++前端check自举入口36–39秒（新旧二进制、新旧C04源码相同），超过测试单命令30秒；HUA_TEST_TIMEOUT_SECONDS=60时array_size_syntax通过，selfhost_types/bytecode/archive、type_contract转为耗尽900秒套件时限，compiler_entry写死30秒。selfhost_parser不限时通过（404秒，限180）；capability_provenance每次分析约29秒（新/旧相同），600秒内跑不完。
- 未执行到：上述时限测试的后段检查。

