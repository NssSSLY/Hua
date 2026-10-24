# HUA-D038第5步第三组接口提案：os exit与平台信息（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，X1–X6全部按推荐（X1退出码0..125、X2展开式退出、X3子任务中调用E4104、X4展开期间再exit为E4104、X5 platform/arch/pid/cpu_count、X6 HUAB16）；开始实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单std.os/process行（P1 `exit`退出码、平台/架构信息；P2参数解析、环境变量列表）；语言设计“A06：公开入口结果与失败边界”（不把int自动变成退出码；Ok→0、Err→1/2/3；入口defer/Task/File清理完成后才报告）；开发指南“A06公开入口”（CLI退出码0/1/2/3）；实现`src/entry.cpp`、`src/vm.cpp`/`src/interpreter.cpp`的`cleanup_error`、`src/task_engines.cpp`。格式沿用[time提案](2026-10-24-time-duration-utc-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 退出码只由入口结果决定：正常/Ok为0，Err为1（`hua.cli`的`usage`为2、`internal`为3），诊断为1、内部故障为3 | src/entry.cpp | `os.exit(n)`是第一个能让程序**指定任意退出码**的接口，要和0–3的既有含义并存（见X1） |
| A06：入口的defer、Result传播与运行结束时的Task/File清理全部完成后才报告；defer里的诊断可替代Err | 语言设计A06 | `exit`不能绕过清理直接结束进程，否则会丢失文件刷新/关闭与defer（见X2） |
| 诊断在VM/解释器里向外传播时由`cleanup_error`运行挂起的defer，入口边界再做Task/File收尾 | src/vm.cpp、src/interpreter.cpp | `exit`可实现为一种**不可捕获的终止信号**沿同一路径展开，复用全部清理 |
| 多处边界把`std::exception`转成E0002内部故障，Task线程把子任务异常带回`await` | src/entry.cpp、src/task_engines.cpp | 终止信号必须原样穿过这些边界，不能被当成内部故障或业务错误（实现风险，验收必测） |
| 产物HUAB跨机器可移植 | 运行时设计 | 平台信息必须是**运行时宿主**的，不是构建机的 |

## 1. `os.exit`（推荐X2-A：展开式退出，`since` 16）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `exit` | `exit(code int) void` | 请求以`code`结束程序：从调用点向外展开，依次运行所有挂起的`defer`、完成入口的Task/File清理（与A06正常结束同序），然后进程以`code`退出，不输出任何错误文本 |

- 范围（X1）：`code`须在0..125，否则E4003（126/127为shell保留，128+为信号约定，Windows/POSIX都安全）。`exit(1)`/`exit(2)`/`exit(3)`与宿主0–3的含义相同或可混淆，文档写明“显式exit的码由程序负责”。
- 展开中的错误：清理期间某个defer产生诊断时，按A06“defer诊断可替代”处理——报告该诊断并按诊断退出（1/3），不再使用`code`；展开期间再次调用`exit`为E4104（避免“哪一个码生效”的歧义）。
- Task（X3）：只允许在主执行（入口）中调用；在`spawn`/`parallel`的子任务中调用为E4104（子任务应返回结果由主任务决定退出）。
- 类型：`exit`的返回类型是`void`，不是“永不返回”，sema不据此省略后续`return`（与当前控制流分析一致；永不返回类型另立）。
- 不受`Result`/`?`影响：它不是Err，不能被`?`、`unwrap_or`等观察到；`check`/`build`不执行，不受影响。

## 2. 平台信息（`since` 16）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `platform` | `platform() string` | 运行时宿主：`"windows"`/`"linux"`/`"macos"`/`"other"` |
| `arch` | `arch() string` | `"x86_64"`/`"aarch64"`/`"x86"`/`"arm"`/`"other"` |
| `pid` | `pid() int` | 当前进程号 |
| `cpu_count` | `cpu_count() int` | 可用逻辑处理器数（`std::thread::hardware_concurrency`，未知时1） |

- 取值来自运行时宿主编译期常量与系统调用：同一个HUAB在Windows与Linux上运行返回各自平台。
- 不提供：环境变量列表与命令行参数解析（P2，另立）、主机名/用户名（涉及隐私与平台差异，后置）、信号（P2）。

## 3. 版本、实现位置与风险

- 新函数`since` 16→HUAB16；不新增类型、不新增opcode。
- 实现：`include/hua/entry.hpp`新增内部终止信号类型（不继承`Diagnostic`），`std.os.exit`抛出它；VM与解释器的`cleanup_error`路径照常运行defer；`command_boundary`/入口边界捕获它、完成清理后返回`code`；Task边界与外部调用边界不得把它转成E0002或业务错误——实施时逐个审计`catch(...)`/`catch(const std::exception&)`位置。
- 平台函数放`src/system_stdlib.cpp`。
- 不改selfhost（自举暂停，L0-4登记）。
- **性能**：这些都不是热点（一次性调用），不建基线基准；验收以正确性与清理顺序为主，`ab_bench`只确认既有基准无退化（新增的边界判断不在每次调用热路径上）。

## 4. 验收

1. 三路径一致（AST解释器、源码VM、无源码HUAB）：`exit(0)`/`exit(7)`的进程退出码与输出；`exit`前后的`print`顺序（之后的代码不执行）；嵌套函数里调用时外层所有`defer`按逆序执行；`exit`之前写入的File被刷新关闭（读回内容）；async main中调用。
2. 错误：码越界E4003；子任务中调用E4104；defer中调用`exit`E4104；defer中诊断替代退出码（按诊断退出）。
3. 平台：`platform()`在本机为`"windows"`、`arch()`为`"x86_64"`、`pid()`与Python子进程观察到的PID一致、`cpu_count()>=1`；HUAB16往返、旧程序格式不变、降级头E6002；`er0_inventory`新增ER0-AA行。
4. CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变；Linux/macOS取值**本机无法验证**，如实标注。

## 5. 需要你决定的点

- **X1 退出码范围**：推荐0..125（避开shell与信号约定）；备选0..255（POSIX完整范围），或任意int（Windows可用32位，但跨平台含义不一）。
- **X2 退出语义**：推荐A展开式（同Python `sys.exit`：运行defer、完成Task/File清理后退出，符合A06清理顺序）；备选B立即退出（同Go `os.Exit`/Rust `process::exit`：不运行defer，只由宿主刷新标准输出，可能丢失未关闭文件的数据）。
- **X3 子任务中调用**：推荐E4104（只允许主执行调用）；备选允许并取消全部任务后退出。
- **X4 展开期间再次exit**：推荐E4104；备选“最后一次生效”或“第一次生效”。
- **X5 平台函数范围**：推荐`platform`/`arch`/`pid`/`cpu_count`；备选再加`hostname`（Result）与环境变量列表`environ`（map）。
- **X6 HUAB16**：推荐新格式号16。

## 6. 实施记录（2026-10-24）

- 全部按定稿实现（std.os新增5个，HUAB16）。`ProgramExit`不继承`std::exception`；解释器块展开对非诊断异常像`return`一样运行defer（defer诊断自然替代），VM新增`cleanup_exit`保持同一语义；入口`finish_async`以取消方式完成Task/File清理。
- 说明：async main的函数体以任务运行，按X3为E4104。诊断展开期间defer里的`exit`被忽略（诊断优先），两执行器一致。
- 观察：首次全量构建的二进制在02_arith上稳定慢约9%，相同源码重建后持平，记为构建布局波动。
