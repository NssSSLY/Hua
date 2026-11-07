# HUA-D038 P1接口提案：剩余六组（2026-11-07用户定稿）

状态：**2026-11-07用户定稿**，A–G全部决定点按推荐，并要求每组完成后提交推送。来源：用户2026-11-07要求“先完成”Unicode大小写/equal_fold、端序pack、模板/千分位格式化、JSON构造、os参数/子进程、缓冲I/O。依据：[标准库路线](../../../../docs/标准库设计与实现路线.md)对齐清单；[接口参考](../../../../docs/标准库与内建接口.md)；[RS-DEC资源决定](../../../../docs/设计决策与未决事项.md)；`src/json.cpp`、`src/file_stdlib.cpp`、`src/text_stdlib.cpp`。

共34个函数，标准库由340增到374。

## 总体安排

| 次序 | 组 | 函数 | `since`/HUAB | 风险 |
| --- | --- | ---: | ---: | --- |
| 1 | 端序pack | 8 | 24 | 低 |
| 2 | 模板/千分位 | 3（另扩展`format`） | 25 | 低 |
| 3 | Unicode大小写/equal_fold | 4 | 26 | 中：要内嵌数据表 |
| 4 | JSON构造/缩进/JSONL | 11 | 27 | 中：HUA-D005首次定语义 |
| 5 | os环境列表/参数解析 | 2 | 28 | 低 |
| 6 | 子进程 | 2 | 28 | 高：HUA-D022首次定语义，新增跨平台宿主代码 |
| 7 | File缓冲读写 | 4 | 29 | 高：改File内部状态与恢复协议 |

每组独立实施与验收：先建基线；三路径测试；ER0；文档；`ab_bench` core/stdlib；reference_bench（子进程只测正确性）；受影响CTest；C2逐字节不变；codex-sync记录。全部完成后再跑一次全量CTest。

## A. 端序pack（`std.encoding`，`since` 24）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `pack_uint` | `pack_uint(v int, width int, order string) Result<Bytes,string>` | `v`须在`0..2^(8×width)-1`，否则Err `ENC_RANGE`；width 8时上限为int的2^63-1 |
| `pack_int` | `pack_int(v int, width int, order string) Result<Bytes,string>` | 补码；须在该宽度有符号范围内，否则Err `ENC_RANGE` |
| `unpack_uint` | `unpack_uint(b Bytes, offset int, width int, order string) Result<int,string>` | 不够字节Err `ENC_TRUNCATED`；width 8且最高位为1时int放不下，Err `ENC_RANGE` |
| `unpack_int` | `unpack_int(b Bytes, offset int, width int, order string) Result<int,string>` | 补码符号扩展；不够字节Err `ENC_TRUNCATED` |
| `pack_f32`/`pack_f64` | `pack_f64(x float, order string) Result<Bytes,string>` | IEEE 754；f32溢出为无穷时Err `ENC_RANGE` |
| `unpack_f32`/`unpack_f64` | `unpack_f64(b Bytes, offset int, order string) Result<float,string>` | 解出NaN/无穷时Err `ENC_RANGE`（Hua float只有有限值）；不够字节Err `ENC_TRUNCATED` |

`width`只能是1/2/4/8，`order`只能是`"le"`/`"be"`，否则E4003；偏移为负是Err `ENC_TRUNCATED`。现有`bytes.u32_le`等保持不变。

- **A1 形态**：推荐上面的按值函数（Go `binary.BigEndian`一类）；备选Python式格式串`pack("<IH", values)`，一次打多个字段，但Hua的Slice同质，int与float混用放不进`[]T`。
- **A2 无符号64位**：推荐超出int范围报Err；备选按位返回负数（调用方易误读）。

## B. 模板与千分位（`std.strings`，`since` 25）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `template` | `template(text string, values map[string]string) Result<string,string>` | `{name}`替换；`{{`/`}}`转义；键缺失或语法错为Err `FORMAT_ARGS`；多余的键忽略 |
| `format_int_grouped` | `format_int_grouped(v int, sep string) string` | 每3位插入`sep`，`-1234567`得`-1,234,567`；含最小int；`sep`须恰为一个标量或空串，否则E4003 |
| `format_float_grouped` | `format_float_grouped(x float, precision int, sep string) string` | 小数位规则同`format_float`，只分组整数部分 |

对齐规格：占位符可写`{key:[[fill]align][width]}`，align为`<`、`>`、`^`，width按标量计，`^`的奇数差额放右侧（与`center`一致）。`template`支持；`strings.format`也扩展支持（`{0:>8}`、`{:*^9}`），这类写法此前一律Err，不影响已成功的调用。

- **B1 format是否一起扩展**：推荐一起（共用同一解析器）；备选只做`template`，`format`保持现状。
- **B2 分组规则**：推荐固定3位、不做本地化；印度等分组留给P2本地化。

## C. Unicode大小写（`std.strings`，`since` 26）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `upper` | `upper(s string) string` | Unicode完整映射，可变长（`ß`→`SS`） |
| `lower` | `lower(s string) string` | Unicode完整映射，含词尾Σ→ς规则 |
| `casefold` | `casefold(s string) string` | Unicode完整折叠（`ß`→`ss`） |
| `equal_fold` | `equal_fold(a string, b string) bool` | 等于`casefold(a)==casefold(b)`，但不分配结果串 |

结果超16 MiB为E4099。不依赖区域设置（土耳其语I不特殊处理）；不做规范化（`é`与`e`+组合符不等）；不提供`title`（依赖分词，留P2）。现有`lower_ascii`/`upper_ascii`保持。

数据：`tests/tools/`下放生成脚本，由Unicode 16.0.0（本机Python 3.14.3的`unicodedata`）生成并提交`src/unicode_case_data.inc`；非ASCII走二分查找，ASCII走快路径。实测约upper 1552项、lower 1460项、fold 1557项，其中207项映射到多码点；估计hua.exe增大约36 KiB，实施时报告实际值。验收以Python `str.upper/lower/casefold`全码点逐一对照。

- **C1 映射**：推荐完整映射（与Python、Rust一致）；备选Go式1:1简单映射（`ß`不变，`equal_fold("ß","SS")`为false，但更小）。
- **C2 版本**：推荐固定16.0.0并写进文档，升级算标准库变更。

## D. JSON构造（`std.json`，`since` 27，HUA-D005）

`JsonData`已是不可变共享节点，持久更新只复制被改的一层、子节点共享，所以`set`/`append`单次O(该层项数)；在循环里逐项构造大数组是O(n²)，大数组应该先备好`[]Json`再用`array`。

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `null` | `null() Json` | JSON null |
| `from` | `from(value T) Result<Json,string>` | 接受范围与`encode`相同；Json原样返回 |
| `array` | `array(items []Json) Json` | 新数组 |
| `object` | `object(fields map[string]Json) Json` | 新对象 |
| `set` | `set(obj Json, key string, value Json) Result<Json,string>` | 返回新对象，原值不变；非对象Err |
| `remove` | `remove(obj Json, key string) Result<Json,string>` | 返回新对象；键不存在Err；非对象Err |
| `append` | `append(arr Json, value Json) Result<Json,string>` | 返回新数组；非数组Err |
| `set_at` | `set_at(arr Json, index int, value Json) Result<Json,string>` | 返回新数组；越界或非数组Err |
| `encode_pretty` | `encode_pretty(value T, indent int) Result<string,string>` | 缩进输出；对象键按UTF-8字节序（同`keys`）；`indent`须为1–16，否则E4003 |
| `decode_lines` | `decode_lines(text string) Result<[]Json,string>` | JSONL；跳过空行；Err消息含行号 |
| `encode_lines` | `encode_lines(items []Json) Result<string,string>` | 每项一行紧凑JSON，以`\n`结尾 |

深度64、100万节点、16 MiB上限沿用，超限在`from`/`encode*`/`decode*`报Err；`set`/`append`不递归统计节点，否则单次不再是O(该层)。

- **D1 更新语义**：推荐持久更新（Json不可变，与现状一致）；备选新增可变`JsonBuilder`句柄（要新增Value种类）。
- **D2 remove不存在的键**：推荐Err（与`get`一致）；备选原样返回。

## E. os环境列表与参数解析（`std.os`，`since` 28）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `environ` | `environ() Result<map[string]string,string>` | 全部环境变量副本；含非法UTF-8的变量Err，不替换 |
| `parse_args` | `parse_args(argv []string, flags []string, options []string) Result<map[string][]string,string>` | 见下 |

`flags`是不带值的名字，`options`是带值的名字，都写成`"verbose"`或`"v"`。支持`--name`、`--name=value`、`--name value`、`-n`、`-n value`；`--`之后全部算位置参数。结果：开关的值是`["true"]`，选项重复出现时按顺序累积，位置参数放在键`""`下。未知名字Err `ARGS_UNKNOWN`，缺值Err `ARGS_VALUE`，开关带`=`值Err `ARGS_VALUE`，同一名字既在`flags`又在`options`为E4003。

- **E1 解析器范围**：推荐上面的最小解析（不合并`-abc`，不生成帮助文本，不做类型转换）；备选`std.flag`完整模块（子命令、帮助、默认值），需要先定声明方式，归P2。

## F. 子进程（新模块`std.process`，`since` 28，HUA-D022）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `run` | `run(argv []string, input Bytes, timeout_ms int) (int, Bytes, Bytes, string)` | 多返回：退出码、stdout、stderr、失败说明；启动子进程、写入`input`、关闭stdin、收完输出后返回 |
| `run_with` | `run_with(argv []string, input Bytes, timeout_ms int, cwd string, env map[string]string) (int, Bytes, Bytes, string)` | 同上；`cwd`为空表示继承；`env`叠加在当前环境上 |

- 不经过shell：`argv[0]`是程序，其余按原样成为参数；Windows按`CommandLineToArgvW`规则转义。`argv`为空E4003。
- 失败说明为空串表示子进程正常跑完，非0退出码不算失败。启动失败时退出码−1，说明`PROCESS_START: ...`。
- `timeout_ms`为0表示不限，负数E4003。超时后杀掉整个进程树（Windows Job Object；POSIX进程组），退出码−1，说明`PROCESS_TIMEOUT`，已收到的输出照常返回。
- stdout、stderr各上限128 MiB，超过时杀进程树，说明`PROCESS_OUTPUT_LIMIT`。
- 输出同时收取，子进程写满管道不会死锁。
- Task内走既有阻塞适配（`builtin_info`已把`process`模块列入`suspends_in_task`），取消时杀进程树并以E4101结束。
- POSIX路径照写，但本机只能验证Windows；文档按惯例标“未验证”。

- **F1 结果形态**：推荐多返回（和`utc_parts`同一机制，不增加Value种类，不影响VM热路径）；备选不透明`process.Output`（像`IOFailure`用查询函数取值，可放进`Result`，但要新增Value种类）。
- **F2 范围**：推荐只做“跑完收结果”；流式管道和长期运行的子进程句柄要新资源类型与合同，这批不做。
- **F3 超时后杀进程树**：推荐杀整棵；备选只杀直接子进程（孙进程可能继续占着管道并卡住读取）。
- **F4 权限**：推荐不加开关（Hua已能读写文件和调用Native）；备选默认禁止、CLI显式开启。

## G. File缓冲读写（`std.file`，`since` 29）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `read_line` | `read_line(h File) Result<string?,IOFailure>` | 读一行，去掉`\n`及其前的`\r`（同`io.read_line`）；EOF为nil；行超16 MiB或非法UTF-8为Err |
| `read_all` | `read_all(h File) Result<Bytes,IOFailure>` | 从当前位置读到EOF；上限128 MiB |
| `read_exact` | `read_exact(h File, count int) Result<Bytes?,IOFailure>` | 恰好读`count`字节；一开始就EOF为nil；中途EOF为Err `unexpected_eof`，`partial`带已读数据 |
| `write_all` | `write_all(h File, data Bytes) Result<void,IOFailure>` | 重试短写到全部写完；失败时`transferred`给已写字节数 |

- 每个File内部加一个最多64 KiB的读缓冲，只有`read_line`填充，逐行读不必每行一次系统调用。
- 缓冲对外不可见：`read`/`read_exact`/`read_all`先消费缓冲；`tell`返回逻辑位置；`seek`丢弃缓冲；`r+`类模式下`write`前先把物理位置调回逻辑位置并丢弃缓冲。
- 不做写缓冲：`write`继续保持“单次写、结果准确”的现有合同；`write_all`是循环，不是缓冲。
- 不改`read`/`write`/`seek`/`tell`的外部合同，HUAB≤28的程序行为不变。

**这组改File内部状态，也要穿过Task恢复协议，我还没有读完`src/resources.cpp`的在途操作部分。** 如果实施时发现缓冲放不进现有协议，我会停下来告诉你，不会绕开合同。

- **G1 位置**：推荐放进File（少一个类型，资源合同不变）；备选新增`file.Reader`句柄（要新增Value种类，还要定它与File的所有权、close和跨Task规则）。
- **G2 通用Reader/Writer**：推荐这批不做，仍后置（RS-DEC-01）：它需要统一stdin/File/Buffer的接口分派，这批做完后清单里的“通用Reader/Writer”仍是未完成；备选现在就做，须先单独设计。
- **G3 写缓冲**：推荐不做；备选加可选写缓冲（大量小写更快，但`write`的进度语义要重定）。

## 不在本批

Unicode规范化/分词/`title`、本地化格式、`std.flag`完整解析、流式子进程、信号、通用Reader/Writer、typed JSON。L0-4的selfhost同步范围随之扩到`since` 9–29，仍待恢复自举时做。

## 实施记录（2026-11-07）

全部完成并逐组提交推送：A c6d08aa、B 9eea2d8、C 71ec8d3、D 272a3a1、E与F 57d22fa（共用since28，一起验收）、G见其提交。各组结果见INDEX中的运行记录。与提案的差异：D组嵌套在构造时就限制64层（防止逐层构造的树释放时压垮宿主栈）；C组hua.exe增大约50 KiB（估计36 KiB）；G组read_line预读计入File额度。提案未写但实施中修复：E/F组`invoke_standard`与`standard_signature`公共路径上的比较造成1–4%退化，已改。
