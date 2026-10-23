# HUA-D038 切片1：字符串P0＋构建器＋parse/format 精确接口提案（待用户审阅，2026-10-23）

状态：**2026-10-23用户定稿**（三点均按推荐：string确认为恒合法UTF-8的不变量；构建器用方案A；格式化用format_int/format_float/pad加只接受string参数的format），进入实施；实施完成前所有函数仍不可调用。审定后写入`docs/标准库与内建接口.md`再实施。依据：现有`std.strings`/`std.buffer`合同、`src/stdlib.cpp`实现约定，以及L0-3基线（`perf/results/win-stdlib-strings-baseline.json`）。

## 0. 沿用的现有约定

- 偏移与长度一律按**UTF-8字节**（与`index`、`slice`、`len`一致）；需要“字符数”的地方明确写“Unicode标量”。
- 空白指ASCII空白：space、tab、CR、LF、VT、FF（与`trim`一致）。
- 单个结果超过16 MiB报运行时`E4099`（与现有strings一致），切分结果超过1,000,000项报`E4099`（与`split`一致）。
- 程序员错误（如负数次数）报运行时`E4003`诊断；只有输入数据本身可能不合法的操作返回`Result<…,string>`，错误串用`前缀: 说明`（与`MATH_DOMAIN:`一致）。
- 全部为纯函数，参数不被修改；返回新值。构建器操作是唯一的`mut`例外。
- 实现：每个函数一次原生调用、单遍、预先计算结果大小一次分配；按现有规则只计调用指令步数，宿主工作量受16 MiB/百万项上限约束。

## 1. 需要你先定的三点

1. **string是否恒为合法UTF-8**：现有所有来源（源码字面量只允许`\\ \" \n \r \t \0`转义、文件/stdin、`bytes.to_text`、`json.decode`）都验证UTF-8，`slice`也拒绝切断字符，但文档未把它写成语言不变量，`rune_count`仍返回Result。**建议确认为不变量**：这样下面按字符计宽度/计数的函数都不必返回Result，构建器也不必重复验证。若不确认，`pad_left`/`pad_right`/`count("")`需改为返回Result。
2. **构建器的形态**：推荐方案A（沿用`Buffer`类型，加两个函数），不新增运行时类型；方案B（新`strings.Builder`类型）API更干净，但要在Value、HUAB类型名、sema、GC里新增一种类型。见第3节。
3. **格式化的形态**：推荐“小而类型化”的一组函数（`format_int`/`format_float`＋`pad_*`＋只接受string参数的`format`模板），不引入可变参数或任意类型参数。见第4节。

## 2. std.strings 新增（P0）

| 函数 | 签名 | 语义 | 对照 | 复杂度 |
| --- | --- | --- | --- | --- |
| `last_index` | `last_index(text string, sub string) int` | 最后一次出现的字节偏移；未找到-1；空`sub`返回`len(text)` | Python `rfind`、Go `LastIndex` | O(n·m)最坏，常见O(n) |
| `count` | `count(text string, sub string) int` | 不重叠出现次数；空`sub`返回Unicode标量数+1（依赖第1点） | Python `count`、Go `Count` | O(n) |
| `repeat` | `repeat(text string, n int) string` | 重复n次；n=0或空文本得空串；n<0为`E4003`；结果超16 MiB为`E4099`（先算长度再分配） | Python `*`、Go `Repeat`、Rust `repeat` | O(结果长度) |
| `split_n` | `split_n(text string, sep string, n int) []string` | 最多n段，最后一段含剩余部分；n=0得空列表；n<0等同`split`；空`sep`按Unicode标量（与`split`一致） | Go `SplitN`、Python `split(sep, n-1)` | O(n) |
| `fields` | `fields(text string) []string` | 按连续ASCII空白切分，不含空项；全空白或空文本得空列表 | Go `Fields`、Python `split()` | O(n) |
| `lines` | `lines(text string) []string` | 按`\n`切分，去掉每行末尾一个`\r`；文本以换行结尾时不产生最后的空行；空文本得空列表 | Rust `lines`、Python `splitlines`（只认\n/\r\n） | O(n) |
| `trim_start` | `trim_start(text string) string` | 只去开头ASCII空白 | Go `TrimLeft`/Rust `trim_start` | O(n) |
| `trim_end` | `trim_end(text string) string` | 只去结尾ASCII空白 | Rust `trim_end`、Python `rstrip` | O(n) |
| `trim_prefix` | `trim_prefix(text string, prefix string) string` | 有该前缀则去掉一次，否则原样 | Go `TrimPrefix`、Python `removeprefix` | O(len(prefix)) |
| `trim_suffix` | `trim_suffix(text string, suffix string) string` | 有该后缀则去掉一次，否则原样 | Go `TrimSuffix`、Python `removesuffix` | O(len(suffix)) |
| `pad_left` | `pad_left(text string, width int, fill string) string` | 左侧用`fill`补到`width`个Unicode标量；已不短于`width`则原样；`fill`必须恰为一个Unicode标量，否则`E4003`；`width`<0为`E4003` | Python `rjust`、Rust `{:>w}` | O(结果长度) |
| `pad_right` | `pad_right(text string, width int, fill string) string` | 同上，右侧补齐 | Python `ljust`、Rust `{:<w}` | 同上 |
| `replace_n` | `replace_n(text string, old string, new string, n int) string` | 左到右替换前n个不重叠匹配；n<0等同`replace`；n=0或`old`为空返回原文（与`replace`一致） | Go `Replace` | O(n) |
| `is_empty` | `is_empty(text string) bool` | 是否为空串 | Rust `is_empty` | O(1) |

不做（留到P1/Unicode）：Unicode大小写、`equal_fold`、`center`、按标量迭代/切片。

## 3. 字符串构建器

基线：`03_concat_loop`（`s = s + "ab"`）Hua 0.106秒，说明唯一持有的字符串已经就地追加；`04_list_join_build`（先收集到List再join）反而更慢（Hua/CPython 6.52倍、Hua/Lua 14.32倍）。所以构建器的价值是：不依赖“唯一持有”这一隐式优化也保证O(n)、追加数字不经临时字符串、少一层Result包装与重复UTF-8验证。

### 方案A（推荐）：沿用`std.buffer`，新增文本快路径

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `write_str` | `buffer.write_str(b mut Buffer, text string) void` | 追加文本字节；不重复验证（依赖第1点），总量超128 MiB为`E4099`（Buffer既有上限） |
| `write_int` | `buffer.write_int(b mut Buffer, value int) void` | 追加十进制表示，不分配临时string |
| `take_text` | `buffer.take_text(b mut Buffer) Result<string,string>` | 把内容移出为string并清空Buffer，不复制；若期间只用过`write_str`/`write_int`/`write_text`则跳过验证，否则验证UTF-8，非法为`Err("TEXT_UTF8: …")`；超16 MiB为Err |
| `reserve` | `buffer.reserve(b mut Buffer, bytes int) void` | 预留容量，不改变长度；超上限为`E4099` |

实现要点：`BufferData`增加“只写过文本”标记，`append`/`write_u8`等字节写入时清除；`take_text`用移动交出存储。现有`write_text`/`text`语义不变。

### 方案B：新`strings.Builder`类型

`strings.builder() Builder`、`strings.append(b mut Builder, text string) void`、`strings.append_int(...)`、`strings.build(b Builder) string`、`strings.builder_len`。好处是API只与文本相关、`build`无Result；代价是新增运行时类型（Value分派、HUAB类型名、宿主sema与Hua编译器签名、GC边、Task快照规则），工作量与风险明显大于A。

## 4. 解析与格式化

现状：`int(string)`/`float(string)`失败时是运行时诊断，不能在程序里处理。

| 函数 | 签名 | 语义 | 对照 |
| --- | --- | --- | --- |
| `parse_int` | `strings.parse_int(text string, base int) Result<int,string>` | `base`为2–36（其他值为`E4003`）；允许一个前导`+`/`-`；不允许空白、下划线和`0x`等前缀；数字大小写不敏感；空串或非法字符为`Err("PARSE_SYNTAX: …")`；超出int64为`Err("PARSE_RANGE: …")` | Go `strconv.ParseInt`、Rust `from_str_radix`、Python `int(s, base)` |
| `parse_float` | `strings.parse_float(text string) Result<float,string>` | 十进制与`e`/`E`指数，允许前导符号；拒绝`inf`/`nan`与上溢（Hua float须有限）为`PARSE_RANGE`；语法错误为`PARSE_SYNTAX`；下溢到0按IEEE就近舍入 | Go `ParseFloat`、Rust `parse::<f64>` |
| `format_int` | `strings.format_int(value int, base int) string` | 按`base`（2–36）输出，小写字母，负数带`-`；`base`越界为`E4003` | Go `FormatInt`、Python `format(v, 'x')` |
| `format_float` | `strings.format_float(value float, decimals int) string` | 固定小数位（0–17，越界`E4003`），四舍六入五成双（与现有`round`一致） | Python `f"{v:.2f}"`、Go `FormatFloat(v,'f',d,64)` |
| `format` | `strings.format(template string, args []string) Result<string,string>` | `{}`按顺序取参数，`{0}`等按下标；`{{`、`}}`为字面量；参数个数或下标不符、未闭合花括号为`Err("FORMAT_ARGS: …")`；参数只接受string（数值先用`format_int`/`format_float`/`str`） | Python `str.format`、Rust `format!`（子集） |

组合示例（说明性）：零填充8位十六进制 = `strings.pad_left(strings.format_int(v, 16), 8, "0")`。

不做（P1以后）：宽度/对齐/千分位等完整格式说明符、科学计数法输出、本地化。

## 5. 性能验收

- 每个P0函数在`perf/stdlib/strings/`有对应基准：现有`07_repeat_pad_manual`、`08_fields_lines_manual`改为调用新函数（保留手写版作对照），新增`last_index/count`、`split_n/replace_n`、`parse/format`、构建器（`write_str`/`take_text`）基准，CPython/Lua用其标准等价函数。
- 目标：上述手写场景改用新函数后同机A/B明显加速；新函数相对CPython的倍数不劣于本次基线中同类已有函数（01/02约3.9倍）；`ab_bench --suite core`与`--suite stdlib`既有项无退化。
- 正确性：AST解释器/源码VM/无源码HUAB三路径一致；空串、空分隔符、非ASCII、`n`为0/负数/超过匹配数、16 MiB与百万项上限、`parse_*`的边界（int64极值、`-0`、指数上下溢）。
- 兼容：新函数登记`since`，与后续批次一起升级HUAB格式；Hua编译器自身的签名表在恢复自举时补（L0-4）。

## 6. 对照数据来源

L0-3基线（另一台Windows机器，CPython 3.14.3、Lua 5.4.7，各7次）：见`docs/标准库设计与实现路线.md`“strings基线”表。
