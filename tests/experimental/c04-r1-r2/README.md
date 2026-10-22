# C04-R1/R2 首轮候选（未进入默认自举）

本目录保存2026-10-20的完整覆盖模块。它实现分析期短ID/256项不可变变换池、返回字段共享、稀疏实参捕获、复制策略和局部分支/循环日志；函数体调度、work/passes、A07与执行预算保持。

本候选没有通过真实负载性能验收，已恢复默认selfhost/capabilities.hua的本轮开始版本。本文件不作为默认入口；不要原样重跑已失败的自身负载。具体结果与失败身份只维护于项目记忆。

本文件须覆盖到同源selfhost源码的独立副本后构建；不是可独立导入的第二套编译器。原始本机源码、组件、命令和日志位于tests/artifacts/c04-r1-r2-20261020。完整过程、限制与下一方案见[项目记忆](../../../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-20-c04-r1-r2.md)。

可独立核对候选的私有合同：

    python -X utf8 tests/capability_transform_tests.py <hua.exe> . <新的输出目录>
    python -X utf8 tests/capability_flow_state_tests.py <hua.exe> . <新的输出目录>
    python -X utf8 tests/capability_instantiation_tests.py <hua.exe> . <新的输出父目录> --source tests/experimental/c04-r1-r2/capabilities.hua

默认路径不包含这些新语义数据结构。所有实验通过也不表示性能放行；来源符号化、写字段稀疏覆盖、依赖组成版本与跨函数重访复用仍待完成。
