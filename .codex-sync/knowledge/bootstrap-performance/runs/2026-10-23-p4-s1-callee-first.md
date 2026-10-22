# P4-S1：C04摘要轮次按被调用方优先访问（2026-10-23）

机器：另一台Windows x64机器（`E:\project\hua`），不是2026-10-22记录所用的机器；计时只在本机内部比较。用户授权实施与验收，验收通过后先审阅再提交。

## 身份

- 基线：main `f5e277a`，`tests/build/hua.exe`在本机重新配置构建（llvm-mingw 20260922、Ninja、Release，sha256 e326943c…）。
- 改动前`selfhost/capabilities.hua`即HEAD版本；改动后sha256 d86f60be…。
- 旧基线67faab1上的同类改动、诊断工具与过程记录在分支`bo11-capability-diagnostics-20261023`（已推送，未合入main）。

## 改动

`selfhost/capabilities.hua`：
- 新增`callee_first(functions, dependents)`：把`s.dependents`（被调用方→调用方）反转为调用边，用显式栈做DFS后序，根和边都按函数键序遍历，结果确定，不受128层调用限制。
- `analyze`：第1轮仍按函数键序（此时才记录调用边）；从第2轮起每轮开头重算顺序，按它访问dirty函数体。32轮上限、未收敛保守回退、最终验证循环（键序、首错不变）都未改。

动机：`functions`是map，按名字字母序遍历；helper的新摘要要等下一轮才传到调用方。旧基线上的测量表明，递归簇在后几轮每轮只多几十到一百多条来源，却每次完整重算约7400万步的函数体。

## 同输入A/B

两份C1都由宿主构建，编译同一份未改动的`selfhost/archive_main.hua`，同机顺序各一次（证据：`tests/build-s1-eval/`，本机不入Git）。

| C1 | 步数 | 墙钟 | CPU | 峰值工作集 | 产物 |
| --- | --- | --- | --- | --- | --- |
| 改动前 | 1,570,130,785 | 228.6秒 | 227.5秒 | 961MiB | 4,171,764字节、CRC32 559473999、sha256 454c7f77… |
| P4-S1 | 1,276,390,981（-18.7%） | 184.3秒（-19.4%） | 183.0秒 | 957MiB | 逐字节相同 |

改动前的步数与f5e277a记录的1,570,130,785完全一致；墙钟与原机343.5秒不同，属于机器差异。

## 完整自举（P4-S1源码）

证据：`tests/build-selfhost-s1/summary.json`、`run.log`、`commands/`（本机不入Git）。

| 阶段 | 结果 |
| --- | --- |
| 宿主构建C1 | 19.2秒，3,642,190字节，sha256 5fcb67aa… |
| C1→C2 | 207.8秒（CPU 206.6），1,281,255,218步，peak_calls 52，峰值964MiB |
| C2→C3 | 202.8秒（CPU 202.3），步数同上，峰值996MiB |
| C2与C3 | 逐字节相同：4,195,418字节、62,599条指令、CRC32 3424058752、sha256 f73869fa… |

比A/B多约490万步，是因为被编译的源码本身多了`callee_first`。

## 正确性

- `tests/capability_c04_tests.py --unbounded`：134例、713检查通过。
- 新旧分析器报告对照（C04工作目录与旧回归目录去重共195个输入）：192个逐字节相同；3个只有`stats.work`变小（`empty_map_element_loop_nested_call` 960→899、`empty_map_element_repeat_mut_call` 831→808、`recursive_field_effect_convergence` 333→309），事实、状态、错误与`capability_passes`全部相同。用户此前已接受这一类差异。
- `capability_instantiation_tests.py` 4例、`capability_metadata_tests.py` 3例/279断言通过。
- 旧基线上另做过能力事实逐条导出比较：capabilities.hua的17,825条事实中，35条标量事实（34个string、1个bool）的来源变少，新为旧的子集，所有可写性标志与首错相同，用户已接受。当前代码上没有重做这一项。

## 回归

指南中的72项CTest：71项通过（388.8秒）。`archives_extensions`失败，与本改动无关：
- 暂存P4-S1后单独重跑，同样失败。
- 原因：其“把第16字节写为0”的变异在本机是空操作。HUAB记录源码路径，本机测试产物3,872字节，载荷长度0x0F00的低字节本来就是0，文件没有变化，于是被正常执行（输出`5\n4 6`）。其余18个变异都正确报E6002。
- 已登记到HUA-D037（测试应确保变异真的改变字节）；未修改该测试。

## 下一步

性能F其余项与C阶段，见`docs/VM与自举性能改造分析.md`第0.3节。旧基线上的S2接口提案（`std.maps`）在上述分支，F1已覆盖其中“相等”，需在当前代码上重估。
