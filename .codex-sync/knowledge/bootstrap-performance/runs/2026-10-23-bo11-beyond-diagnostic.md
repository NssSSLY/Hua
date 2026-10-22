# BO-11超5亿诊断与HUA-D008新反例（2026-10-23）

## 结论

1. 候选A已回退：`git checkout`恢复`selfhost/capabilities.hua`（820b86a0…）和`tests/capability_instantiation_tests.py`（94568482…）。C0重建为875fb8dd，与回退前基线逐字节相同。
2. capabilities.hua单模块能力分析实际需要931937087步，即生产预算的1.86倍。共9轮，logical work 103576。statement每次访问自第5轮起稳定在约74M步：第5–8次访问分别为74.09M、72.69M、74.19M、74.40M。
3. 运行没有得到完整编译。Hua编译器报`E3003@698:102 [capabilities.hua] read-only location`，在types阶段之后结束，所以降级、验证、编码阶段的步骤都未计入总数1030005824。原生`hua check`接受该文件。
4. 这是Hua分析器相对原生的假阳性，独立于性能阻塞C07。已登记为HUA-D008 P0。

## 运行

- 工具：`tests/bootstrap_capability_sample.cpp`新增第7个参数`--beyond-production-diagnostic`。只有带该标志才接受≥5亿的步骤上限；不带标志时，6亿上限被拒绝（返回2）。这只是诊断工具的入口，不改变生产CLI或`compiler`档位。
- 限制：步骤2×10¹⁰，调用128，GC 65536；监督墙钟上限7200秒。
- 复核：带标志运行type_rules，6497566步，HUAB与普通运行逐字节相同。
- 结果：总计1030005824步、381.34秒（CPU 378.8秒）、峰值工作集540450816字节、峰值调用47。各阶段：parse 56344087（8次进入）、names 11138208、types 24997501、return-infer 5588864、capabilities 931937087（361.8秒）。
- 证据：`build/bo11-beyond-20261023/`（`summary.json`、`events.jsonl`、`commands/capabilities-beyond.*`、`repro/`）。

## 反例

| 文件 | 内容 | 原生 | Hua分析器（C04驱动，当前源码） |
| --- | --- | --- | --- |
| r1 | `let args=list.snapshot(items)`；`var out=args[0]`；分支内`out.root.w=true`（编译器原模式） | OK | E3003 read-only location |
| r2 | `fn f(args []R) R { var out=args[0]; out.root.w=true; return out }` | OK | E3003 callee writes through a read-only member/view |
| r3 | main内`var out=a[0]; out.root.w=true` | OK | 通过 |
| r4 | `fn f(args []R)`内的单层写`out.w=true` | OK | 通过 |
| r5 | `fn f(arg R) R { var out=arg; out.root.w=true; return out }`，以字面量调用 | OK，运行输出true | E3003 callee writes through a read-only member/view |

判断：对记录参数（或参数元素）的本地副本写嵌套成员时，Hua分析器把这次写记入函数摘要，作为对参数路径的写；调用方实参只读时就报错。单层写正确，说明`copy_view`的record_copy只作用于根和直接字段，嵌套字段视图仍保留参数来源。这是基于反例行为的推断，修复前应对照`copied()`/`copy_view`/`put`/`summary`中的写来源确认。

## 下一步

1. 修复HUA-D008反例，把r1/r2/r5（以及r3/r4作对照）加入`tests/data/a07_completion_cases.json`，跑C04、实例化/元数据隔离和5个真实输入的HUAB对照。
2. 修复后重跑本诊断，确认capabilities.hua能完整编译，并重新测能力阶段步骤。假阳性可能放大了摘要中的参数来源，这一点要实测，不能预设。
3. 再按实测量级选择性能路线：原生集合运算（约54%步骤在集合/视图循环）、路径截断加集合上限的抽象域，或两者组合。改动生产预算仍需用户决定。
