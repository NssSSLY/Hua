# Hua 项目背景

Hua 是轻量系统脚本语言原型。当前入口是 README.md 和 docs/文档目录.md；学习与规则确认使用 docs/语言百科与设计核对.md，实际能力、最近验收和限制看 docs/项目概览与进度.md，设计合同以对应专题文档为准。

主要目录：src/ 为 C++20 前端、语义、VM 和标准库实现；include/hua/ 为接口；selfhost/ 为 Hua 自举起步；examples/、tests/ 为示例和回归；scripts/ 为构建与安装入口；editors/ 为 VS Code 集成。依赖 CMake、Wasmtime，Python Bridge 是可选插件，核心不依赖 Python。

本目录只记录便于换电脑继续工作的摘要，不替代九份权威文档。docs/history/ 保留历史原字节，历史建议不是任务授权。项目身份使用 project.json 中稳定的 UUID，不依赖电脑盘符。

2026-10-08新增长期方向：公共Typed HIR/Hua MIR驱动开发VM、CPU原生缓存和发布AOT；可选HHR/AEE处理合格计算的设备/数据/成本。E0六类43条合同已冻结；E1先单模块int/float/bool/void、小控制流/普通函数与旧字节码降级。E1最小IR已完成内部实现；完整IR/恢复帧/AOT/GPU未实现，性能目标与设备API后置，当前栈VM/HUAB6/ABI1不变。权威设计见docs/设计决策与未决事项.md，百科18.06/23.02/31.04/34.03是对应学习入口。

2026-10-08 E0合同入口：docs/语言设计与类型规则.md负责数值/错误，docs/运行时与扩展设计.md负责所有权/Task恢复/预算/兼容；设计决策ADR-HUA-CONTRACT-001负责结论，百科0.5保留36章/207条。当前实现定向核对31场景/194断言通过，不证明新机制实现。

2026-10-09归并下一代方向：Simple Code, Adaptive Execution, Explicit Control。HXE协调程序VM/原生路径，Flow复用Task/资源，HHR是异构底座、AEE是计算成本策略；Smart Memory/Explain/Capability/Replay/组件包分发/UI状态迁移逐步建立。E1a–d为最近实现任务；E2→E3单CPU后端→E4缓存/发布，E5/E6a可配合推进后E6b单显式GPU→E7自动策略。完整规划只维护设计决策文档integrated-implementation-plan，百科0.6仍207条。


2026-10-09 E1a–d已交付：include/hua/ir.hpp与src/hir.cpp、mir.cpp、ir_verify.cpp、ir_bytecode.cpp；内部tests/ir_driver.cpp生成hua_ir_probe，默认main/旧编译器/VM/HUAB6/ABI1保持。强类型稠密ID只保证同一模块修订的确定性，不是编辑后永久身份；HIR/MIR及ScalarConstant不保存AST指针。MIR是带失败/预算/取消记录的栈序SSA，直接生成旧指令，桥接仅保活旧函数声明。

E1范围单模块int/float/bool/void、绑定/名称赋值、显式标量签名普通函数、if/短路/while/int范围for/退出、标量print/int/float；不覆盖尺寸类型/容器/Result/泛型实例/defer/Task/导入。unsupported整体初始化前E8001，非法内部IR/桥接E8002；旧CLI能力继续可用。内部Explain区分证明与Runtime检查，不提供性能结论。

E1阶段历史证据：完整24/24（116.27秒，Windows x64 Release，Python Bridge ON，CTest并行2）；Bridge OFF独立构建IR定向2/2（15.09秒）；44项C++检查、37四路径场景/10类拒绝/779断言；两构建示例与HUAB及3个无CPython DLL导入通过。913本地链接、36章207条/207确认栏、43 E0合同和38 history文件原值通过。详见docs/运行时与扩展设计.md#e1-ir-runtime、docs/项目概览与进度.md#e1-verification、docs/开发与运行指南.md#e1-internal-tool；下一步E2a，不自动开始E2或提交/推送。

2026-10-09五份材料的历史设计归并阶段，非代码任务：docs/设计决策与未决事项.md#design-synthesis-20261009综合网络、轻量Core/SDK/Extensions与Result错误增强；统一实施仍以integrated-implementation-plan为唯一入口。ER错误切片服务S路线，N网络接E5/E6a，D发行解耦为E4服务；S1b＋ER0为近期应用，E2a继续编译主线。百科0.8只补30条，207确认栏与43 E0规则保持；catch/标准Error/Result<void>/入口报告仍待确认且未实现。当前Wasmtime必选，默认Result错误为string，旧CLI/HUAB6/ABI1/Task快照与取消合同不变。

2026-10-09待完成设计维护入口（错误审阅准备时历史记录）：docs/设计决策与未决事项.md#design-backlog，HUA-D001–D033固定ID。助手判断优先级/影响/依赖，用户选择执行；新增设计/需求提炼更新同一列表，完成保留证据不重编号。#error-foundation-review是当前ER0/ER1审阅入口，8项推荐均待确认；本阶段只文档，未完成接口盘点或实现Error/Result<void>。

2026-10-09最新：用户确认ER-DEC-01–08并明确授权执行ER0/ER1基础。原125标准接口、38内建、13Python源码审阅完成；Result<void,E>无载荷ok()及std.error.Value只读名义值16接口已实现，当前16模块/141函数。旧默认string/125签名/Diagnostic/取消/预算/defer/ABI1保留；普通HUAB6、新Error/void能力7，读1–7，旧Runtime拒绝7且普通6可用。最终完整26/26及35/179已通过；百科0.9/207条确认原值/E0 43/history38保持。未提交推送，不自动启动ER2–ER4/其他HUA-D项。权威入口error-foundation-review、er0-interface-audit、er1-confirmed-contract、er1-runtime、er-foundation-verification。

2026-10-09远程交付授权：用户要求推送当前累积工作到NssSSLY/Hua main，包含S1a/E0/E1/ER0/ER1及相关文档；普通推送不覆盖远程。过往未提交记录仅描述当时状态，最终交付以Git HEAD与origin/main实际核对为准。
