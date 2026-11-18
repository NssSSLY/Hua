# N2.1–N2.2 Windows 单计算线程原生 Task

用户在 2026-11-18 批准首个里程碑。实现范围见[原生 Task 运行时](../../../../docs/运行时与扩展设计.md#native-task-single-worker)。工作树基线为 perf/vm-register-r1 的 0fba914，Windows 11 x64，Intel Core i5-11300H，Clang 23.1.2，Release，Python bridge 关闭。验收宿主 hua.exe SHA-256 为 90d61441256e34125360fe39bcf83fd405d6a69b7a6c1c8265b56b55e5a07736。

## 验收

- native_backend 通过，用时 128.12 秒，92 个程序与 VM 的 stdout、stderr 和退出码相同，15 个合法但超出当前原生实现范围的程序在生成前拒绝。新增 5 个可执行场景，包括单 worker 嵌套等待、字符串和精确数值快照、异步入口、80 次完成登记和重复等待，以及休眠后原生错误的调用轨迹。父子用例通过 worker_id 比较物理线程，两个子任务的输出各出现一次。
- VM 的 task_resume_single 和 task_resume_migration 通过。并行构建原生差分时，task_resume_public 的历史用例用 10 ms 等待推测子体已登记 defer，随后在 1 ms 请求取消。此时出现取消前子体尚未登记清理的输出。N1 基线在同一构建负载下复现，N2 在原生编译结束后独立执行通过（5.59 秒）。保留两次失败输出。阶段未重跑完整自举。
- 初次原生生成用例的 C++ 编译发现字符串引用类型不匹配。修正参数后，专项及最终原生差分均通过。无新任务执行超时。

## 同机性能

同期交错运行，发布计步关闭。同步程序沿用跨语言仓库实验的 hot.hua 和 N1 热基准可执行程序。每个运行先执行完整热身，再经 stdin 放行，对 n-1、n-2 和 n-3 执行三次计算，以读到正确总校验值作结束。首对样本预热，后续各 11 个样本计入。

| 样本任务 | N1 中位数 | N2 中位数 | N1 / N2 样本范围（ms） |
|---|---:|---:|---|
| 约 300 万次单循环，三次热载荷 | 52.237 ms | 52.161 ms | 51.060–62.832 / 51.244–59.412 |
| fib(30) 热身，三次递归计算 | 9.389 ms | 9.309 ms | 7.603–10.543 / 7.804–9.904 |

同一个 N2 原生程序的定时器对照，固定 1 个后台 worker，8 个独立 Task 各 sleep(50)，两种走法均返回总和 8。先创建全部再 await 的冷进程中位耗时 77.089 ms，逐个创建并立即 await 为 510.812 ms，各 7 次。范围分别为 75.075–110.698 ms 和 504.807–654.356 ms。约 6.63 倍是等待重叠的实测，包含启动及收尾。同步内核两组中位差均在 1% 内，完整自举、文件吞吐和多核工作量另行测量。

原始证据位于 tests/artifacts/n2-windows-20261118/，含 ctest.log、baseline-task-resume-under-load.log、task-resume-isolated.log、sync-performance.csv、scheduled-sleep.csv 及 bench.log。N1 热基准与 N2 热基准可执行文件的 SHA-256 同时写入 bench.log。
