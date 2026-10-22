# Hua项目背景

项目：C:/file/project/hua，NssSSLY/Hua，project_id=aab00f84-5a35-48b3-973b-4ccd29e3d31c。语言基础A→基础标准库B→编译器自举C。C01–C06、C07容器/样例已有；当前完整自身与代际未验收，子集通过不代表G1/G2/G3。

唯一进度docs/项目概览与进度.md；方案docs/设计决策与未决事项.md#bootstrap-recovery-plan；百科逐项确认。仅10份活文档（含VM与自举性能改造分析，2026-10-22确认）；history/用户确认不改；过程只.codex-sync/knowledge；构建tests/build或tests/build-*，原始日志tests/artifacts，诊断tests/tools。selfhost是正式模块。

当前默认源码直接自举已测：14模块288120字节/C0 e3f8a429/对应VM库3afa32e1，实际进入main一次，165.063秒先达500M步，C04未完无HUAB。C04占总步72.21%，301次后续函数体访问占C04工作81.70%，同一体每次工作扩大；完整失败后未重跑。时间含跟踪，不能与旧普通基准比较。初始化前工具错误独立记录。

下一项先固定最贵目标的调用摘要/写效果变化，区分必要新事实与重复传播，再选传播单位改造。只省物化/相等或CPU、先重写VM，没有足以改变本次步骤失败的证据。实验候选宿主准备120秒是不同路径，不是当前默认阻塞。5亿步与600秒的规模合同如需更改应单列核定，不能宣称语言逻辑必然不能自举。

默认源码/核心/history不改，数字候选仍非默认。小校准普通/观察64348步、回执/产物相同，reader/输出3通过；不代表自身成功。本轮只诊断/文档、未性能改码、未提交推送。最新过程knowledge/bootstrap-performance/runs/2026-10-20-direct-selfhost-trace.md及-evidence.json；原始tests/artifacts/selfhost-trace-20261020-run。
