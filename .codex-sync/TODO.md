# 待办与下一次选择

更新2026-10-22。完整自举已成功并到达代际不动点（555.6秒，gen2逐字节相同），阻塞解除；Windows完整CTest已逐项定因，未发现本轮回归；步骤3a后自举474.3秒。唯一完整清单docs/设计决策与未决事项.md；性能工作日志docs/VM与自举性能改造分析.md第0节；最新过程knowledge/bootstrap-performance/runs/2026-10-21-c04-false-positives-selfhost.md。

| 次序 | 待办 | 验收 |
|---|---|---|
| P1/中 | 宿主前端检查自举入口36–39秒 | 9项测试在自带30/60/180/600/900秒时限内跑不完（compiler_entry写死30秒）；定是否调测试时限或加速sema，然后补跑后段检查 |
| P2 | 3项早已存在的断言 | selfhost_ast_pool期望E4007实为E4099；frontend_extended compiler_main.hua E8201@14:11；lowering capability_passes上限函数数+2 |
| P0/高 | 性能路线（docs/VM与自举性能改造分析.md第9节与0.3节） | A、B、D2机制、F1已完成（10-23，同时段自举470→344秒）；下一步F其余项与C共享字符串/16字节Value→D1结构COW→E→G；每子项ab_bench＋完整自举逐字节相同 |
| 待决定 | GC计数合同 | 无环对象不入环检测会改变std.gc.collect()计数，需用户确认后才能做 |
| 待决定 | 第9.5节合同问题 | 融合计步方式、非原子引用计数、map迭代、寄存器VM时机 |
| 待决定 | Windows堆分配 | operator new约13%＋系统释放；是否引入更快分配器（如mimalloc）由用户决定 |
| P1/高 | 原生集合操作（聚合相等、std.map） | 减少C04步数；AST/VM/HUAB三路径对照 |
| P1 | C09覆盖/兼容、A12/B10、G3 | 自举成功后按原路线推进 |

当前没有提交或推送；需用户明确要求。
