# Hua 自举编译器标量 async/await（2026-11-18）

HUA-D039 第 2 项。用原 SDK 中的 C1 种子构建修改后的 Hua 编译器源码，再运行生成的 C2 编译同一份冻结源码。验收复用原 Windows x64 宿主和 VM 任务原语。C1 的 SHA-256 为 `0ed1ee238c5e6866dac1d5580691238b9ff6d1eb517e4b703d90fad9f3439624`。

| 代 | 墙钟 | VM 步数 | 调用深度峰值 |
|---|---:|---:|---:|
| C2 | 522.106 秒 | 1,149,035,535 | 52 |
| C3 | 524.478 秒 | 1,150,080,922 | 52 |

每代监督上限 600 秒。C2 与 C3 逐字节一致，SHA-256 为 `e1a5d9c6fef93b9f796a3d9ec80fc40baaeb17485c72a88268a892ea39b7c5d6`。两份产物通过宿主 reader，分别通过 3 个正例和 6 个反例。覆盖 `await twice(21)`、显式 `Task<int>`、重复等待只执行一次、bool/float/string/void 以及两个模块的同名异步函数。示例去除源码后用格式 6 的 HUAB 运行，输出与源码 VM 一致。拒绝项覆盖非 Task 操作数、错误返回类型、容器参数、异步入口、函数外等待和嵌套异步函数。失败保留旧输出。

首个纯 Hua 模块 `std/alg_integer.hua` 通过普通模块导入测试。旧种子生成的 HUAB 在移走项目和模块源码后，7 组整数下界与现有 `std.alg.lower_bound` 一致，并验证重复项输入保持原值。SDK 的标准命名空间接入另行验收。

阶段计量工具通过 6 项汇总自检。本次记录完整代际墙钟与 VM 预算，完整负载的内部分段归因待执行。命令、冻结源文件哈希、回执与预算存于 `tests/artifacts/selfhost-scalar-async-generations-20261118`。复验脚本为 [selfhost_async_bootstrap.py](../../../../tests/selfhost_async_bootstrap.py)。
