# Windows SDK 源码标准模块验收

宿主和 Hua 自举编译器均支持 `import std.alg_integer as alg`。SDK manifest 以 `source_modules` 登记 `src/std/alg_integer.hua`，诊断和 HUAB 使用 `@sdk/src/std/alg_integer.hua` 作为逻辑身份。两端验证清单和真实路径，从指定 SDK 根解析。项目中的同名模块无法遮蔽该实现。

在仓库外带空格的独立目录验收。两套编译器的产物对 7 组下界输入给出一致结果，同时验证空切片、重复项、近 int64 边界值、末尾插入点与输入保持原值。移走项目和 SDK 模块源码、清除 HUA_HOME 后，两份 HUAB 6 仍可运行。仅修改 Hua 函数实现并重建，两个产物都输出更新结果。缺失清单、源码或 SDK 根、错误参数、Windows 目录联接越界均有码可查，原有输出保留。直接调用种子时错误来自回执，PowerShell 编译入口将其映射为非零退出码。

编译器本体使用 `std.fs.canonical`，种子选择 HUAB 17，独立小例也验证了此能力版本。使用源码库的普通程序将函数链接进 HUAB 6，Native ABI 1。旧 C3 产物通过新版宿主读取检查。新版种子通过标量 Task 的 9 个回归用例。

本地 SDK 位于 `tests/artifacts/p0-stdlib-20261118/Hua P0 Release SDK`。清单全部 24 项尺寸和 SHA-256 已核对。宿主 SHA-256 为 `bd3dfeb7cc2f3c93876651fd70f0b6cd1fc33c9243270bdf5abc2e15f127cd51`，种子 SHA-256 为 `bd05b626e10cc534cbb7b0854dd7e640d6e1a6b5e9f2714305340622bc8ea595`。哈希用于产物验收，编译器使用由调用者选择的可信 SDK 根。复验及用法见[开发与运行指南](../../../../docs/开发与运行指南.md#windows-sdk-source-stdlib)。
