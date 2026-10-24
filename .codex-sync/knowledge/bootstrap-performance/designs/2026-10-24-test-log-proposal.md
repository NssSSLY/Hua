# HUA-D038第5步第五组接口提案：test/log（与io stderr）（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，T1–T7全部按推荐（T1 `@test`＋`hua test`、T2断言失败为诊断E4900、T3 logfmt、T4每个测试全新上下文、T5首批只支持源码、T6纳入`io.write_err`、T7 HUAB18）；先建基线再实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单std.test/log（候选）行（P1 `assert`系列与最小测试运行器、结构日志level/fields；P2 benchmark工具）与std.fs/path/io行的P0缺口“stderr写出”；语言设计“defer和attribute”（attribute集合封闭）；`src/parser.cpp:97`（attribute白名单）；`src/main.cpp`（CLI命令）。格式沿用[fs提案](2026-10-24-fs-walk-copy-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| attribute只接受`@entry`/`@doc`/`@deprecated`/`@inline`/`@noinline`，其余在Parser拒绝 | src/parser.cpp:97 | 测试发现需要新增`@test`（一处语言级改动，见T1） |
| 函数值类型是名义的`fn:名字`，不同函数无法组成同一类型的Slice | 语言设计 | 纯库式`test.run([t1, t2])`做不到，测试发现只能由宿主按attribute完成 |
| CLI只有`check/build/run/interpret/ast/bytecode` | src/main.cpp | 需要新命令`hua test` |
| `std.io.write`只写stdout；没有任何写stderr的接口 | 接口文档std.io | 日志与测试报告需要stderr；顺带补上P0缺口 |
| 诊断（含E4xxx）会中止程序；业务失败用Result | 语言设计A06/E0-E01 | 断言失败适合做成诊断，测试运行器逐个捕获 |
| HUAB保留函数与签名但不保留`@doc`等源码元数据 | 运行时设计 | `hua test`首批只支持源码（见T5） |

## 1. `std.io`补stderr（`since` 18）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `write_err` | `write_err(text string) Result<int,string>` | 原样写stderr（无自动换行），成功值为字节数；与`write`对称 |

## 2. `std.test`与`@test`（推荐T1-A，`since` 18）

- 新attribute `@test`：只能用于顶层、零参数、返回`void`或`Result<void>`/`Result<void,E>`的函数（否则E2002/E3004）；`hua run/interpret`忽略它（测试函数仍是普通函数，可被调用）。
- 新命令`hua test <file.hua> [--filter 子串]`：按源码顺序逐个运行`@test`函数；**每个测试在全新的执行上下文中运行**（重新执行模块初始化，互不共享全局状态、Task、文件）；测试函数结束即通过，返回Err、抛出诊断（含断言失败）、超步数/调用深度即失败；输出每个测试一行`PASS name`/`FAIL name: 消息 (文件:行:列)`与汇总`N passed, M failed`，全部通过退出0，否则退出1。
- 断言函数（失败时抛诊断E4900 `assertion failed: ...`，在测试中由运行器记为失败，在普通程序中按诊断结束）：

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `assert` | `assert(condition bool, message string) void` | 条件为false时失败，消息原样附带 |
| `equal` | `equal(actual T, expected T) void` | 按语言`==`比较（结构相等）；失败消息含两边的打印形式（各截断到200字节） |
| `not_equal` | `not_equal(actual T, other T) void` | 相等时失败 |
| `fail` | `fail(message string) void` | 无条件失败 |

`equal/not_equal`的两个参数须同型（静态E3004）；不可比较的种类（函数、Task、File、Buffer等）沿用`==`的E4008。

## 3. `std.log`：结构日志（`since` 18）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `debug` / `info` / `warn` / `error` | `(message string) void` | 写一行到stderr |
| `with_fields` | `with_fields(level string, message string, fields map[string]string) void` | 带字段的一行；level为`debug/info/warn/error`（否则E4003） |
| `set_level` | `set_level(level string) void` | 进程级最低输出级别，默认`info`；低于它的调用不写也不格式化 |

- 行格式（T3）：`ts=2026-10-24T10:17:03.123Z level=info msg="..." key=value`（logfmt）；值含空格、`=`、`"`或控制字符时加引号并转义；字段按键的字节序输出（map本就有序）。时间来自`time.unix_ms`同一时钟，格式与`time.format_rfc3339`一致。
- 每次调用一次原生写入（整行一次`write`，Task间不交错）；不缓冲（崩溃前的日志不丢）。

## 4. 版本与实现位置

- 新函数`since` 18→HUAB18；`@test`只影响源码与`hua test`，不进入HUAB（T5）。
- 实现：Parser白名单加`test`，sema检查`@test`函数形态；`src/main.cpp`新增`test`命令（复用`run`的模块加载，逐个测试新建VM与RuntimeContext）；`src/system_stdlib.cpp`加`io.write_err`、`std.log`；`std.test`断言放`src/stdlib.cpp`或新文件。
- 不改selfhost（自举暂停，L0-4登记）。

## 5. 基准

- log/01 log_manual（`io.write_err`＋手写`format_rfc3339`拼logfmt，10万行，stderr重定向到文件）→ native `log.info`/`with_fields`；参照Python `logging`（同格式Formatter）。`set_level`过滤掉的调用另测一次开销。
- test/断言不是热点，不建基准。

## 6. 验收

1. `hua test`：通过/失败/Err返回/诊断/超步数各一例的输出与退出码；`--filter`；测试间全局状态隔离（一个测试改全局，另一个看不到）；非法`@test`（有参数、非顶层、返回int）的静态报错；`run`仍正常执行同一文件的main。
2. 断言：消息内容与截断、结构相等的struct/map/List、不可比较种类E4008、普通程序中断言失败按诊断退出1。
3. log：四个级别的格式、引号与转义、字段顺序、`set_level`过滤、Task中并发写入行不交错（多任务各写1000行后逐行解析）。
4. `io.write_err`与`io.write`分别到stderr/stdout；三路径一致（测试运行器只支持源码，断言/log/write_err三路径都测）；HUAB18往返、降级头E6002；`er0_inventory`新增ER0-AC行。
5. CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变。

## 7. 需要你决定的点

- **T1 测试发现**：推荐A——新增`@test` attribute＋`hua test`命令（与Go/Rust/pytest同类，是一处语言级改动）；备选B——不改语言，只提供断言函数，用户自己在main里调用各测试并汇总（无隔离、失败即中止）。
- **T2 断言失败的形态**：推荐抛诊断E4900（测试中被运行器捕获为失败；普通程序中即中止）；备选返回`Result<void,string>`（需要`?`传播，写法更啰嗦）。
- **T3 日志格式**：推荐logfmt文本（人读友好，机器可解析）；备选JSON Lines，或两者都提供并用`log.set_format`切换。
- **T4 测试隔离**：推荐每个测试全新执行上下文（重新初始化模块）；备选同一上下文顺序运行（更快但测试间可能互相影响）。
- **T5 HUAB支持`hua test`**：推荐首批只支持源码（`@test`不进HUAB）；备选同时扩展HUAB记录测试函数列表（需要新增产物字段）。
- **T6 io.write_err**：推荐纳入本组（补P0缺口，日志依赖它）。
- **T7 HUAB18**：推荐新格式号18。

## 8. 实施记录（2026-10-24）

- 实现：`src/report_stdlib.cpp`（`io.write_err`、`std.test`、`std.log`，一把stderr锁保证整行写入）；Parser白名单加`test`（struct上E2002），sema检查`@test`形态（E3004，含非顶层）；`src/main.cpp`新增`hua test`（逐个测试新建RuntimeContext与VM，`VirtualMachine::set_entry`指定入口，`reset_report_state`复位日志级别）；`since` 18/HUAB18。
- 与提案的细化：`with_fields`的字段键也校验（空键或含空格/`=`/引号/控制字符为E4003）；值的转义另含`\r`与`\u00XX`；返回Err的测试报告为`FAIL 名称: returned Err: 消息`（无位置）；调用`os.exit`报告为`test called os.exit(n)`。
- 验证：`tests/test_log_tests.py`（57项：报告/过滤/隔离、断言三路径与截断、日志格式/转义/字段顺序/set_level、3个Task各300行不交错、write_err与write分流、错误码、HUAB18与降级E6002）；ER0-AC01–AC11。基准与验收结果见[标准库路线](../../../../docs/标准库设计与实现路线.md)“第5步第五组test/log结果”。
