# 当前状态

更新：2026-10-11。实际项目C:\file\project\hua；HEAD与本地origin/main仍8d365fba75018361255f8f8ac7255219f57f69ec。本轮未提交/推送，全部此前未提交工作保留。

## 当前用户目标与完成范围

用户授权保持A07正确性，以C04自身分析/降级为真实负载，优化符号、位置、Value/List/Result和环境复制，再决定VM架构。本批实现了局部成本优化与固定输入实测，没有开展寄存器VM/JIT或新后端。

- C03私有环境按模块缓存身份、Pool和有序导入；C04以模块ID索引Unit，函数/构造器声明由已有表解析，分支环境保留普通数据及函数值/构造器值别名。
- AST span构造/替换核对已知行区间，公共span仍upper_bound、UTF-8字节列/EOF/来源及形状验证保持。
- C++有序Struct复制插入提示/容器预留；List已验证元数据复用，Result少一层复制，AST/VM绑定/字段/返回边界减少重复复制。新增已消费Value重载逐层检查独占；共享结构/Result仍snapshot，固定数组/deep clone/资源/只读子项/深度错误不变。
- 实测发现原C04将for弃值位“_”记录为空名字符号，只在该存储入口修正；原失败报告保留，去除唯一错误行后所有名称/类型/能力/来源/Core字段与优化版相同。最终完整报告与第一批优化字节一致。

## A07保护及兼容

A07-1的C++sema和Hua来源/remember/join/narrow核心保持；原27例492检查两构建继续通过，旧HUAB E4007/no stdout保持。合法标量副本/新字段替换/clone规则保留；完整跨函数逐字段效果、未知别名、混合能力字面量与多返回精度仍待，不是整个A07/G1完成。

500M步骤/128调用/GC65536/600秒监督/16MiB报告和普通百万步/GC256不变；GC算法、Task/资源/Value布局、HUAB6/7/8、reader1–8及Native ABI1不变。17标准模块153函数38内建和108个SB1标准签名保持；通用JSON仍1000000节点/64深度/16MiB。没有硬RSS或联合Task预算。

## 固定工作负载测量

13文件214439字节输入与执行分析的编译器分别冻结；before-summary与after-summary source_hashes完全相同。同输出模式（均完整Core验证、只输出摘要）528.406→343.859秒，分析累计408.278→243.655秒，降级累计527.233→342.662秒，步骤234124909→196434038，分配66073729→40694222（减少38.4%）。峰值工作集1313505280→1313669120字节约相同，不能宣称RSS同步下降。

最终完整报告401.235秒/201310397步/51调用/峰值1754341376字节；报告14786227字节、44416 Core节点，通过正式校验。原完整报告的墙钟/峰值未保存，不补造；原for错误行单独说明。单次成对样本＋最终完整报告不替代稳定分布、跨语言或跨平台基准。

## 当前自身链路与架构决定

正式当前源码13文件215190字节，分析累计256.302秒、降级364.608秒、生成403.520秒、独立指令验证481.405秒；505.687秒正常退出但报告status:error、E8202 JSON nesting/node limit exceeded。259341841步/51调用/峰值2052702208字节；198字节stdout是错误报告，未执行自身指令/适配HUAB。类型报告固定浅层，结合src/json.cpp核对为1000000节点上限；精确最终报告节点/指令数未导出。

上一轮A07历史链路600.109秒止于验证（13文件214439字节），不能与本次不同输入直接换算加速倍数。本次原监督和步骤预算没有触发，剩余阻塞转到报告传输/序列化。

决定保留现有栈VM，暂不启动布局/寄存器/JIT/GC大改。下一先核C06有界编译产物表示/传输与生产者、验证器、加载器共同合同（分块或C07二进制等候选），不能仅拆json.encode绕过公共上限而让消费者继续失败。独立执行、C07完整Hua HUAB/C08连续代际/C09覆盖/C10扩大规模及G1/G2/G3仍待；用户选择后实施。

## 本批回归与真实限制

最终Windows x64 Release Bridge ON全量41/41、694.62秒；OFF全量39/40、690.12秒，filesystem_limits replacement visibility失败，随后该项独立连续5次通过。第一批ON也曾39/40同项失败、独立5次通过，第一批OFF39/39。失败原因未定位，不把独立通过抹掉全量失败，B04继续跟踪；该项代码与测试未改。

两配置A07 27/492、C04 63/892、A05 27/305、C05 42/1705、C06 63/2306；新增Value独占/共享/嵌套/只读/Result/Multi/固定数组/clone/void/GC/depth21项通过。旧产物两配置E4007/no stdout。没有POSIX或其他架构实测。

## 证据、文档及接手

build/c04-performance保存基线与固定输入、完整/摘要结果及阶段、paired-summary-comparison.json、equivalence-results.json、regression-results.json及CTest原件、measurement-environment.json、final-own-results.json及失败原件。它们是忽略的本机测量证据，不随Git分发。tests/selfhost_pipeline_budget_tests.py为新手动测量工具，tests/selfhost_budget_tests.py复现完整当前自身链路。

只改五相关活文档、四上下文与新检查点；39份history原字节、36章207条/207确认、E0 43行、ER8/RS14、32任务与D001–D034保护。完整保护/链接结果见build/c04-performance/document-verification.json。权威进度docs/项目概览与进度.md#c04-pipeline-verification、决策#c04-pipeline-performance、运行时#consumed-value-copy、指南#c04-performance-usage。

A/B未整体验收，B不等待C。A07剩余、A09/A10深clone、A02尺寸身份及C07所需B03/B04/B05仍按用户选择推进；不自动开始未选后端/网络/数据库，未请求不提交推送；不复制原生Codex会话、数据库或CODEX_HOME。
