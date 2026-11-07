# 2026-11-07 HUA-D038 P1 os参数与子进程（since28，HUA-D022）

- 任务：按[六组提案](../designs/2026-11-07-p1-remaining-six-groups-proposal.md)E、F组实现`os.environ/parse_args`与新模块`process.run/run_with`。两组共用since28，一起验收和提交。
- 身份：272a3a1同工具链Release为基线（tests/build-e28-base）。输入：`perf/stdlib/os/01–02`。
- 正确性：`tests/os_process_tests.py`63项，三路径，子进程是真实的Python进程：stdin/stdout/stderr字节保真；两路各13 MiB同时输出、4 MiB stdin不死锁；14种特殊参数（空串、引号、反斜杠、UTF-8、shell元字符）原样到达；启动失败、超时返回已收输出；超时、Task取消后孙进程确实被杀（tasklist核对）；子进程退出后留下的后代不会卡住读取；128 MiB输出上限；cwd与env叠加。
- 实现细节：Windows用Job Object（KILL_ON_JOB_CLOSE）与PROC_THREAD_ATTRIBUTE_HANDLE_LIST，并发Task只继承各自的管道。子进程退出时后代一并结束，所以不能用`run`启动常驻后台进程（已写入接口文档）。POSIX路径（fork/exec、进程组、poll）照写，本机未编译未运行。
- 测试夹具的两处修正：Windows把空字符串环境变量视为删除，去掉该断言；子进程Python按控制台代码页输出，加`reconfigure(encoding="utf-8")`。
- 性能（各7次）：参数解析手写0.065→原生0.037秒（1.8x），Hua/CPython 1.73x→1.01x，Hua/Lua 4.54x→2.63x（`win-stdlib-args-baseline.json`、`win-stdlib-args.json`）。子进程只测正确性。
- **回归与修复**：首轮stdlib 129项中位0.986x、11项低于0.96x。base复制对照0.99–1.01x而new 0.96–0.99x，是真实的1–4%退化，不是噪声。原因是把`f.module=="process"`放在`invoke_standard`公共路径前部、把`f.since==28&&f.module=="os"`放在`standard_signature`最前面，每次标准调用都多付一次比较。改为先比整数`since`、把签名分支移到alg/list热路径之后，复测恢复到0.99–1.00x。修复后全量重跑：core 0.99–1.02x，stdlib 129项中位1.003x，最低0.949x（alg/13、maps/07，未复测）（`win-process-core-ab.json`、`win-process-stdlib-ab.json`）。
- 自举：C2逐字节不变（f73869fa…，1,281,255,218步）。未跑全量CTest（六组完成后统一跑）；POSIX未验证。
