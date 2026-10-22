# BO-11来源增长定位（2026-10-23）

## 结论与边界

仅诊断，未修改Hua编译器源码或运行时，未放行。完整自身0次、代际0次，生产预算保持，未提交推送。证据目录`build/bo11-origin-flow-20261023/`：`summary.json`、`write-origin-composition.json`、`modules-events.jsonl.origins.json`、`capabilities-events.jsonl.origins.json`、`capabilities-writes-events.jsonl.origins.json`、`run.log`、`run-writes.log`。

1. capabilities输入超出5亿步骤的原因已定位：statement↔expression↔expression_body递归簇（analyze调用它们）的摘要写来源每轮增长，927→3940→13619→29348，第5轮在诊断上限处被截断。同时脏函数体176→99→63→44→13，数量在收敛，但每轮处理的来源集合更大。Modules每轮写来源最多292，5轮收敛。
2. 否决假设：没有发现无界嵌套改写。来源中`!`的个数恒为2，最长184字符。instantiate_view只替换所有者，路径前缀最多保留一层`@被调用方!位置`。
3. 组成：statement第4轮6653个不同写来源中，96.3%是被调用方新分配来源按调用点重标后的结果（`statement!capabilities.hua:<调用节点>|@<被调用方>!<分配位置>#@<字段路径>`）。共35个调用点，每点平均183条，只来自46个被调用方分配身份，其余规模全部是字段路径展开。expression_body第5轮97.5%、18个调用点、每点314条。
4. 字段路径展开的来源：`seeded()`以`fn!位置`建立新来源，`field_defaults`再沿8层字段投影出每条路径。Environment 272、State 188、Value 87字段宽度相乘后，每个新分配身份都对应数百条路径，而每个调用点都持有一份完整副本。
5. 成本位置：分支合并（`merge_view via merge`，statement第4轮14221次调用、787545条输出来源）、出口合并（`via summary`）、调用实例化（`expression_body->expression`等）。这些都按来源集合大小线性处理，所以集合逐轮增长时，同一函数体每次访问的步骤随之增长。

## 修复候选（均未实施，待用户选择）

| 候选 | statement第4轮不同写来源 | 语义影响 | 风险 |
| --- | --- | --- | --- |
| A 在（调用点，被调用方分配身份）内去掉字段路径后缀 | 6653→811 | 同一调用结果内不同字段的新分配会被`same_storage`视为可能别名，比现在保守 | 可能产生新的弱更新、拒绝写入或事实变化；需C04逻辑对照及事实差分 |
| B 每个调用点只保留1个新来源 | 6653→282 | 同一调用结果内所有新分配都视为可能别名 | 最粗，诊断变化风险最大 |
| C 保留现有标签，共享/驻留每调用点集合或增量合并 | 不变 | 无 | 只减常数，增长阶数不变；需要表示层工作，收益未证明 |

A/B会改变别名精度，属于能力分析语义层面的取舍，不能当作纯性能修复直接实施。无论选哪项，都要先验证：C04 184例逻辑对照、实例化隔离、Modules/Parser等完成输入的事实全量差分，再看capabilities.hua单模块是否在5亿步骤内完成。

## 方法与身份

- `tests/bootstrap_capability_sample.cpp`新增来源流记录：按“轮次|helper|调用方->被调用方 via 父helper”累计调用数、输入/输出来源数、最大输出、最长字符串和`!`嵌套；按“轮次|函数体”记录摘要返回/写来源、字段数、是否变化；最后一轮statement/expression*/analyze的写来源全文另存。轮次的判定：函数体声明序回退即为新一轮；`probing=false`的访问记为final。
- 编译器`875fb8dd`与2026-10-22相同，运行时`e7eb25b8`。Modules观察输出与普通HUAB逐字节相同，163043745步。capabilities两次都在499999999步诊断上限停止，用时约2分51秒。
- 第一次脚本化编辑因shell转义未写入，生成的旧exe已删除；改用Edit后重建。没有负载失败。
