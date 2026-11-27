# Hua VS Code 插件

支持 `.hua` 语法高亮（`#` 行注释与 `#* ... *#` 嵌套块注释）、内置 LSP 语言服务器（`hua lsp`）驱动的语义着色、多错误容错实时诊断、跳转定义（同文件与跨模块）、成员与全部 32 模块 / 422 项标准库智能补全、悬停类型/签名提示（Hover）、文件大纲（Outline）、代码片段，以及 `Hua: Check/Run/Build Current File`、`Hua: Restart Language Server` 命令。

安装 `tests/build/hua-language-0.3.0.vsix`；设置 `hua.compilerPath` 为已安装的 `hua.exe`，或先将安装 `bin` 加入 `PATH` 并重启 VS Code。程序参数在 `hua.runArguments` 配置，LSP 开关与协议日志可在 `hua.enableLsp` / `hua.lspTrace` 调整。

LSP 模式在可信工作区中直接读取编辑器内存态缓冲区（无需先保存即可实时检查语法、类型、能力与跨模块导入错误），支持一次报告多处语法错误并保留其余部分的着色与补全。当 LSP 关闭或编译器不可用时，自动回退到保存后 `hua check` 检查；未信任工作区只提供声明式编辑与基础语法高亮。

当前已覆盖实时多错诊断、语义高亮、跳转定义、成员/标准库补全、Hover 与大纲；重命名重构、格式化与断点调试将在后续阶段补充。未发布 Marketplace。


## 运行你写的第一个程序

保存main.hua，按Ctrl+Shift+P选择Hua: Run Current File；输出在任务终端。
也可以点击“终端 → 新建终端”，在文件所在目录输入：

```powershell
hua run .\main.hua
```

终端找不到hua时，本机可用：

```powershell
& "C:\file\project\hua\tests\build\hua.exe" run .\main.hua
```

hua.compilerPath仅配置插件，不设置终端PATH。安装步骤、Hello Hua示例及字节码运行见 [第一个程序怎么运行](../../docs/开发与运行指南.md)。
