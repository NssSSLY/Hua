# Hua 项目背景

Hua 是轻量系统脚本语言原型。当前入口是 README.md 和 docs/文档目录.md；学习与规则确认使用 docs/语言百科与设计核对.md，实际能力、最近验收和限制看 docs/项目概览与进度.md，设计合同以对应专题文档为准。

主要目录：src/ 为 C++20 前端、语义、VM 和标准库实现；include/hua/ 为接口；selfhost/ 为 Hua 自举起步；examples/、tests/ 为示例和回归；scripts/ 为构建与安装入口；editors/ 为 VS Code 集成。依赖 CMake、Wasmtime，Python Bridge 是可选插件，核心不依赖 Python。

本目录只记录便于换电脑继续工作的摘要，不替代九份权威文档。docs/history/ 保留历史原字节，历史建议不是任务授权。项目身份使用 project.json 中稳定的 UUID，不依赖电脑盘符。
