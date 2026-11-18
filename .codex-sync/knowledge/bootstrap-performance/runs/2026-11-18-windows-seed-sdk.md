# Windows 可搬移种子 SDK 验收（2026-11-18）

HUA-D039 第 1 项。用 Windows x64 LLVM-MinGW Release 宿主装配，Python Bridge 关闭。宿主来自工作树已有的构建 tests/build-n2-windows-20261118，工作树基线为 0fba914，包含既有 N2 改动。SDK 附 Wasmtime 49.0.2 DLL 和两份许可证。

| 文件 | SHA-256 |
|---|---|
| 宿主 hua.exe | 90d61441256e34125360fe39bcf83fd405d6a69b7a6c1c8265b56b55e5a07736 |
| Wasmtime DLL | 2867257c069c5fa01f2a823bdb66c9ae3ab98dfcea016991cfd0f709f35fc837 |
| C1 种子 | 0ed1ee238c5e6866dac1d5580691238b9ff6d1eb517e4b703d90fad9f3439624 |
| manifest.json | 27a4b197141909a18c3052081001ffe7feef9a3294952b7693cc834dce9b50b5 |
| C2、C3 | f73869fa341b7e9e76fd754edee0d36b2accd175a058c6d6fa243360ce0f0def |

从包内 14 个 Hua 模块，由包内宿主生成 HUAB 7、ABI 1 的 C1 种子。部署副本与测试工作目录都位于仓库外，目录名含空格。子进程 PATH 仅有 Windows System32，清除 HUA_HOME 和 HUA_PATH。复制后核对清单内全部 23 个文件的大小及哈希。种子编译两模块样例，迁走原源码路径后执行 HUAB，输出 `2 2`。缺 Wasmtime DLL、缺编译器种子均给出依赖路径和退出码 1。

| 完整编译 | 墙钟 | VM 预算步 | 峰值调用深度 |
|---|---:|---:|---:|
| C2 | 500.430 秒 | 1,132,389,837 | 52 |
| C3 | 496.315 秒 | 1,132,389,837 | 52 |

每代命令的墙钟上限为 600 秒。两代用相同包内源码，退出码 0，回执一致。字节逐一相同，产物为 HUAB 7、4,195,418 字节、62,599 条指令、CRC32 3424058752。迁走编译器源码后，两个产物均通过宿主 reader 检查，各自仍能编译同一示例，生成物与 C1 的样例字节一致且运行输出一致。

复现见[开发与运行指南](../../../../docs/开发与运行指南.md#windows-seed-sdk)。本机原始记录位于 tests/artifacts/windows-sdk-20261118-relocated，包括逐条命令、stdout、stderr 和 report.json。计时来自各代一次完整运行。解析、C04 等内部阶段成本仍归后续性能归因。
