# C04 数字事实直接查询候选

状态：实现切片与隔离合同通过，**未放行默认**。最终完整对照在宿主静态语义准备120秒停止，没有最终分析器/实际规模性能结果。此前中间版 Parser 总步骤回退19.38%，已停止扩大测试；不能把中间测量当最终修正版结果。

实现：共享形状的 FieldId→[TypeId, capability bits] 固定数组；来源基底与操作表分离；查询直接叠加 patches/cuts；复制策略按需组合；完整规范形状比较加精确来源/差量比较。局部ID不能跨表比较。兼容写入/未知/超深边界仍可规范化完整表。不是完整 OriginId DAG 或已完成的增量编译器。

默认 selfhost/capabilities.hua 与上一轮 c04-symbolic 均保持原样。将本文件复制到同源32模块目录才是显式试验；最终快照及身份见过程摘要。不要复用中间 analyzer-compact.huab 代表最终源码，更不能将分析器作为编译器种子。

运行隔离合同：

    python tests/capability_numeric_tests.py tests/build/hua.exe . tests/artifacts/<新目录>
    python tests/capability_symbolic_tests.py tests/build/hua.exe . tests/artifacts/<新目录> --source tests/experimental/c04-direct/capabilities.hua

下一项：将事实池移出递归 Value 表示，先验证会话句柄、能力隔离与宿主准备规模，再恢复完整语义与实际负载对照。编译/执行预算及 A07 不变。

[过程、失败与证据边界](../../../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-20-c04-direct.md)。
