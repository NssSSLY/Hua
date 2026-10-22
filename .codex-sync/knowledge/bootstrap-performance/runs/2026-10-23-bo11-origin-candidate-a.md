# BO-11候选A：按调用点去掉新分配来源字段路径（2026-10-23）

## 结论

用户授权实施候选A并做正确性与预算检查。正确性检查全部通过，性能目标失败：capabilities.hua单模块仍超出5亿步骤。改动保留在工作区，未提交、未推送、未回退，是否回退待用户决定。建议回退：它降低了同一新调用结果内字段间的别名精度，却没有带来收益。完整自身0次、代际0次，生产预算保持。证据目录`build/bo11-origin-a-20261023/`（`summary.json`、`real-inputs.json`、`logical-parity.json`、`commands/`、`before/`原文件、`regressions/`）。

## 改动

- `selfhost/capabilities.hua` `instantiate_view`：所有者含`!`的来源（被调用方新分配）重标为`调用方!模块:节点`后，路径只保留`@分配身份`（已有`@x!y#@...`时截到第一个`#@`前），去掉字段路径；多个来源合并为同一键时只读标志取或。非新分配来源行为不变。
- `tests/capability_instantiation_tests.py`：两条预期从`|@other!u#@tail`、`|@original#@leaf`改为`|@other!u`、`|@original`，按字节修改。第一次脚本写入改变了行尾，已用原文件恢复后重做。
- 改动前原文件：`before/capabilities.hua`（820b86a0…）、`before/capability_instantiation_tests.py`（94568482…）。新C0 `0d03c9ad…`，运行时`e7eb25b8`不变。

## 正确性

| 检查 | 结果 |
| --- | --- |
| 实例化预言（4例 × AST/源码VM/无源码HUAB） | 通过；旧源码按新预言失败，作负对照 |
| C04完整套件（`--unbounded`） | 124例/667检查通过 |
| 新旧分析器报告对照（C04输入与C07回归输入去重共185个） | 逐字节相同，报告SHA aeb73c19… |
| 元数据隔离 | 3例/225断言通过 |
| 5个真实输入编译产物 | type_rules/ast_pool/Parser/Modules/留出HUAB均与875fb8dd输出逐字节相同 |

报告只含节点`type_name/writable`等事实，不含来源。所以“报告相同”证明的是这些输入上的可见诊断与事实不变，不证明所有程序的别名判定都不变。

## 性能

| 项目 | 改动前 | 候选A |
| --- | --- | --- |
| capabilities.hua普通运行 | E4099（10-18记录） | E4099，159.02秒、453.4MiB、峰值调用47 |
| statement第4轮不同写来源 | 6653 | 1239 |
| statement逐次访问步骤 | 1.91M/8.05M/34.64M/55.03M | 1.91M/12.90M/43.65M/66.34M |
| statement最后一次访问处理的视图来源条目 | 841909 | 2047589 |
| instantiate_view / merge_view独占步骤（诊断上限内） | 72.7M / 42.9M | 64.7M / 42.6M |
| same_flags / note_prefix | 27.0M / 31.7M | 28.3M / 33.9M |
| 5个真实输入步骤 | 基线 | +0.003%–+0.05% |

## 原因判断

- 成本由每次访问实际处理的“视图×来源”条目决定。宽值（Environment/State/Value）的每个字段视图各带一份来源集合，在分支合并、出口合并、调用实例化中被逐一处理。不同标签数减少不会降低这个乘积。
- 调用方内部的`projected()`/`field_defaults`又会为调用结果重建字段路径：调用点来源992个中有430个重新带上字段路径。
- statement访问步骤反而上升，具体机制未单独证明。可能与合并后视图更早在更多字段间共享相同来源、使合并/比较处理更多条目有关，属未证推测。

## 下一步建议

1. 回退候选A（`git checkout -- selfhost/capabilities.hua tests/capability_instantiation_tests.py`），恢复原别名精度。
2. 针对“视图×来源”条目设计修复，候选方向（未评估）：同一值的字段视图共享来源集合引用，而不是每字段复制；`merge_view`/`same_flags`在集合引用相同时短路；或把来源从字段视图提升到值级、按需投影。先用有界样本证明条目数与步骤下降，再做同一组正确性检查。
