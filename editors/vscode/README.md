# Hua VS Code 基础插件

支持 .hua 语法高亮（# 注释与嵌套块注释）、括号/注释切换、片段、保存后检查和 Hua: Check/Run/Build Current File 命令。

安装 build/hua-language-0.1.0.vsix；设置 hua.compilerPath 为已安装的 hua.exe，或先将安装 bin 加入 PATH 并重启 VS Code。程序参数在 hua.runArguments 配置。

检查使用已保存源码，诊断定位包含导入模块，UTF-8 字节列转换为编辑器 UTF-16 列。修改后清除旧检查，保存后重查。运行在 VS Code 任务终端，stdin 可交互；参数以进程参数传递。未信任项目只提供声明式编辑功能。

这是基础插件，没有 LSP、补全类型推断、跳转/重命名、格式化或断点调试。未发布 Marketplace；当前验证包含模拟 API 和真实 VS Code CLI 隔离安装，不等同于交互界面验收。


## 运行你写的第一个程序

保存main.hua，按Ctrl+Shift+P选择Hua: Run Current File；输出在任务终端。
也可以点击“终端 → 新建终端”，在文件所在目录输入：

```powershell
hua run .\main.hua
```

终端找不到hua时，本机可用：

```powershell
& "C:\file\project\hua\build-python-off\hua.exe" run .\main.hua
```

hua.compilerPath仅配置插件，不设置终端PATH。安装步骤、Hello Hua示例及字节码运行见 [第一个程序怎么运行](../../docs/开发与运行指南.md)。
