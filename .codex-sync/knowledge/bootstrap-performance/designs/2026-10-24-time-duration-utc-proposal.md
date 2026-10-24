# HUA-D038第5步第二组接口提案：Duration与UTC日期（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，T1–T7全部按推荐（T1 int纳秒、T2函数作用于Unix毫秒、T3多返回utc_parts、T4提供format_rfc3339_offset、T5毫秒非0才输出、T6拒绝闰秒、T7 HUAB15）；先建基线再实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单std.time行（P0 `Duration`构造/加减/比较/毫秒秒换算；P1 UTC日期时间结构、RFC 3339格式化与解析；P2时区、本地化）与“时间：Duration明确单位，Deadline用本进程单调时钟，现实时间用于日志/恢复”；现有合同`docs/标准库与内建接口.md`“std.time”“时钟：std.time，2接口”；实现`src/system_stdlib.cpp`、`src/tasks.cpp`。格式沿用[Deque/Heap提案](2026-10-24-collections-deque-heap-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 已有`time.monotonic_ns() int`（进程内单调纳秒）与`time.unix_ms() int`（Unix现实毫秒） | src/system_stdlib.cpp | 时长单位已是int纳秒、现实时间已是int毫秒；新接口沿用这两个单位 |
| `task.sleep(ms int)`、`task.timeout(t, ms int)`收int毫秒（0..86400000） | src/tasks.cpp:11 | 新Duration若是独立类型，与现有任务接口不能直接配合 |
| Hua没有运算符重载、类型别名与Tuple；有多返回与`_`数字分隔 | 语言设计 | 独立Duration类型只能用函数做加减比较（每次一次标准调用，比int运算慢一个数量级）；日期分量可用多返回一次取出 |
| 业务失败用Result；整数溢出为E4002 | 语言设计E0-E01 | 解析失败为Result；构造越界为Result（可预期的输入错误） |

## 1. 时长：int纳秒约定（推荐T1-A，`since` 15）

时长就是int纳秒（与`monotonic_ns`同单位，Go `time.Duration`的表示），用普通`+ - < ==`运算；毫秒交给`task.sleep`时写`d // 1_000_000`。只补必须由库提供、手写容易错的部分：

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `since_ns` | `since_ns(start int) int` | `monotonic_ns() - start`（计时惯用法一次调用） |
| `format_duration` | `format_duration(ns int) string` | 与Go `Duration.String()`逐字相同：`1h30m0s`、`1.5s`、`250ms`、`2µs`、`100ns`、`0s`、负数前缀`-`；µs用U+00B5 |
| `parse_duration` | `parse_duration(text string) Result<int,string>` | Go `time.ParseDuration`语法：可带符号，若干“十进制数（可带小数）＋单位”，单位`ns`/`us`/`µs`/`μs`/`ms`/`s`/`m`/`h`；`"0"`可省单位；语法错Err `TIME_SYNTAX`，超出int64纳秒Err `TIME_RANGE` |

## 2. UTC日期：函数作用于int Unix毫秒（推荐T2-A，`since` 15）

现实时间点就是`unix_ms`同单位的int；公历为前推格里高利历（proleptic Gregorian，与RFC 3339/ISO 8601一致），年份范围0001–9999，不计闰秒（Unix时间）。

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `utc_parts` | `utc_parts(ms int) (int, int, int, int, int, int, int)` | 多返回年、月(1–12)、日(1–31)、时、分、秒、毫秒；`let y, mo, d, h, mi, s, frac = time.utc_parts(t)`；超出0001–9999为E4003 |
| `utc_from_parts` | `utc_from_parts(year int, month int, day int, hour int, minute int, second int, millis int) Result<int,string>` | 逐项校验（含闰年2月29日）后得到Unix毫秒；非法分量Err `TIME_RANGE` |
| `weekday` | `weekday(ms int) int` | ISO星期：1=周一 … 7=周日 |
| `day_of_year` | `day_of_year(ms int) int` | 1–366 |
| `is_leap_year` | `is_leap_year(year int) bool` | 格里高利闰年规则 |
| `days_in_month` | `days_in_month(year int, month int) int` | 28–31；月份越界E4003 |
| `format_rfc3339` | `format_rfc3339(ms int) string` | `YYYY-MM-DDTHH:MM:SSZ`，毫秒非0时为`.mmm`三位：`2026-10-24T05:22:15.123Z` |
| `format_rfc3339_offset` | `format_rfc3339_offset(ms int, offset_minutes int) string` | 按固定偏移显示（不查时区库）：`2026-10-24T13:22:15+08:00`；偏移须在−1439..1439（E4003） |
| `parse_rfc3339` | `parse_rfc3339(text string) Result<int,string>` | RFC 3339 `date-time`：`T`或`t`分隔、`Z`/`z`或`±HH:MM`偏移、可选小数秒（1–9位，截断到毫秒），换算为UTC毫秒；秒为60（闰秒）、非法日期、缺偏移均Err `TIME_SYNTAX`/`TIME_RANGE` |

实现：Howard Hinnant `days_from_civil`/`civil_from_days`算法（纯整数、全部O(1)、无系统时区调用，跨平台逐位一致）。

## 3. 版本与实现位置

- 新函数`since` 15→HUAB15；不新增类型、不新增opcode。
- 实现：`src/time_stdlib.cpp`（时长格式/解析与公历换算）；`std.time`在`invoke_standard`入口早分派（同random/deque，轻量调用）。
- 不改selfhost（自举暂停，L0-4登记）。

## 4. 基准（实施前先测手写基线；`perf/stdlib/time`）

- time/01 civil_manual（Hua手写`civil_from_days`拆分＋`strings.pad_left`拼RFC 3339）→ native `format_rfc3339`/`utc_parts`；参照Python `datetime.fromtimestamp(..., timezone.utc).isoformat()`、Lua `os.date("!%Y-%m-%dT%H:%M:%SZ")`。
- time/02 parse_manual（`strings.slice`＋`parse_int`手写解析）→ native `parse_rfc3339`；参照Python `datetime.fromisoformat`、Lua手写。
- time/03 duration_manual（手写`format_duration`的简化版）→ native；参照Python手写Go格式化。
- 每步`ab_bench`（core与stdlib既有）无退化，可疑项多份二进制交替复测。

## 5. 验收

1. 三路径一致；与Python `datetime`（UTC）在0001-01-01、1969-12-31T23:59:59.999、1970-01-01、闰年2月29日、1900/2000世纪规则、9999-12-31上的分量与格式逐项一致；`weekday`/`day_of_year`一致。
2. `format_duration`/`parse_duration`与Go参考输出（测试脚本内置Go算法的Python移植）一致：0、各单位边界、负数、最小int64、小数`1.5h`、`1h2m3.004005006s`往返；溢出Err。
3. `parse_rfc3339`：大小写`T/Z`、偏移换算、小数1–9位、错误分量与闰秒Err、缺偏移Err。
4. HUAB15往返、旧程序格式不变、降级头E6002；`er0_inventory`新增ER0-Z行。
5. 基准表写入路线文档；CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变。

## 6. 需要你决定的点

- **T1 时长形态**：推荐A——int纳秒约定（可直接用运算符，零开销，与`monotonic_ns`一致）；备选B——新不透明类型`time.Duration`（单位不会被误用，但加减比较都要函数调用，且`task.sleep/timeout`要再加Duration版本）。
- **T2 日期形态**：推荐A——函数作用于int Unix毫秒，分量用多返回一次取出；备选B——新类型`time.DateTime`（需要原生构造可读字段的值，工作量与Deque相当，且仍无运算符）。
- **T3 日期分量取法**：推荐多返回`utc_parts`（一次换算）；备选每个分量一个函数（`year(ms)`等，每次重新换算）。
- **T4 偏移显示**：推荐提供`format_rfc3339_offset`（固定分钟偏移，不涉及时区库）；备选只提供UTC，偏移留到时区设计。
- **T5 格式化精度**：推荐毫秒非0才输出`.mmm`；备选总是输出`.mmm`（定长，便于排序比较）。
- **T6 闰秒**：推荐`parse_rfc3339`拒绝秒=60（Unix时间无闰秒）；备选接受并折算为下一秒的0秒。
- **T7 HUAB15**：推荐新格式号15。

## 7. 实施记录（2026-10-24）

- 全部按定稿实现（std.time新增12个，HUAB15），无接口差异；`std.time`新增函数在`invoke_standard`入口早分派；`monotonic_ns`与`since_ns`改为共用`monotonic_now_ns`。
- 结果表见docs/标准库设计与实现路线.md“第5步第二组time结果”：格式化3.2x、解析12x、时长格式化12.6x。
