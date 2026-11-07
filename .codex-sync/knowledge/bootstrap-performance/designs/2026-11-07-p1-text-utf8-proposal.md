# HUA-D038 P1接口提案：strings center/码点与encoding UTF-8（2026-11-07用户定稿）

状态：**2026-11-07用户定稿**，W1–W7全部按推荐（W1奇数差额放右、W2 codepoints与chars都提供、W3 slice_runes越界Err、W4非法码点Err不替换、W5 utf8_decode多返回失败(-1, 0)、W6文本↔码点归strings且Bytes↔UTF-8归encoding、W7 HUAB23）；先建基线再实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单std.strings行P1（`center`、按码点迭代/切片）与std.encoding行P1（UTF-8校验与码点编码）；[接口参考](../../../../docs/标准库与内建接口.md)std.strings/std.bytes/std.encoding；`src/text_stdlib.cpp`、`src/encoding_stdlib.cpp`。

不在本组：Unicode大小写与`equal_fold`（需要Unicode数据表，单独定版本与体积）、通用端序pack/unpack（格式设计独立）、模板/千分位格式化、按行缓冲读取。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| string恒为合法UTF-8（语言不变量，2026-10-23确认）；`bytes.to_text`失败为Err、不替换 | 接口文档std.bytes、CURRENT_STATE | `codepoints`/`chars`不需要Result |
| 没有char类型；码点只能用`int`，单个字符只能用`string` | 语言设计 | 同时需要`[]int`与`[]string`两种取法（见W2） |
| `strings.slice`、`index`、`len`都按字节；切断字符为Err | 接口文档std.strings | 码点切片用新名字，不改旧函数 |
| `rune_count`返回`Result<int,string>`，“标量”指Unicode标量值 | 接口文档std.strings | 新函数沿用“标量”口径，不数字形簇 |
| `pad_left`/`pad_right`宽度按标量计，fill须恰为一个标量且`width>=0`，否则E4003 | `text_stdlib.cpp` | `center`完全沿用 |
| 文本上限16 MiB、Slice上限100万项，超限E4099 | `text_stdlib.cpp` | 16 MiB文本可超过100万标量，`codepoints`/`chars`须报E4099 |
| Err前缀已有`ENC_SYNTAX`、`MATH_DOMAIN`等 | `encoding_stdlib.cpp` | 新Err沿用“前缀: 说明”格式 |
| 当前工作区有两批未提交切片，`since`已用到22 | `src/stdlib.cpp` | 本组为`since` 23；实施前先决定那两批是否提交（见第5节末尾） |

## 1. 接口（`since` 23）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `strings.center` | `center(s string, width int, fill string) string` | 两侧用`fill`补到`width`个标量；奇数差额多出的一个放右侧；已不短则原样；`fill`须恰为一个标量、`width>=0`，否则E4003；结果超16 MiB为E4099 |
| `strings.codepoints` | `codepoints(s string) []int` | 全部Unicode标量值，按出现顺序；超100万项E4099；结果只读 |
| `strings.chars` | `chars(s string) []string` | 每个标量一个string，按出现顺序；超100万项E4099；结果只读 |
| `strings.from_codepoints` | `from_codepoints(cs []int) Result<string,string>` | 编码为UTF-8；含代理区D800–DFFF或超出0–10FFFF的项为Err `TEXT_CODEPOINT: ...`，消息含第一个非法项的下标；结果超16 MiB为E4099 |
| `strings.slice_runes` | `slice_runes(s string, start int, end int) Result<string,string>` | 按标量下标的半开区间；负数、`start>end`、越界为Err，与`strings.slice`规则一致；单次调用O(end)，不缓存下标 |
| `encoding.utf8_valid` | `utf8_valid(b Bytes) bool` | 严格校验（拒绝过长编码、代理区、超范围），与`bytes.to_text`判定一致 |
| `encoding.utf8_encode` | `utf8_encode(cp int) Result<Bytes,string>` | 单个标量编码为1–4字节；非法为Err `ENC_RANGE: ...` |
| `encoding.utf8_decode` | `utf8_decode(b Bytes, offset int) (int, int)` | 多返回：从字节偏移解码一个标量，得到码点与字节数；偏移越界或编码非法时为`(-1, 0)`，不报错，便于扫描 |

例：

```hua
import std.strings as strings
import std.encoding as encoding
import std.bytes as bytes
fn main() {
    print(strings.center("ab", 5, "*"))                   # *ab**
    print(strings.codepoints("a中"))                      # [97, 20013]
    print(strings.chars("a中"))                           # [a, 中]
    print(unwrap(strings.from_codepoints([97, 20013])))   # a中
    print(unwrap(strings.slice_runes("a中文b", 1, 3)))    # 中文
    let cp, size = encoding.utf8_decode(bytes.from_text("中"), 0)
    print(cp, size)                                       # 20013 3
}
```

## 2. 实现与性能

- 全部放在现有`src/text_stdlib.cpp`与`src/encoding_stdlib.cpp`；无新opcode、语言类型或ABI变化。UTF-8编解码共用一组内部函数，校验与现有`text_utf8_error`判定一致并以测试对照。
- `center`：与`pad_left`共用参数检查，一次预留结果长度。
- `codepoints`：先按非续字节数得到项数并检查上限，再一次预留。
- `chars`：单字节结果走短字符串，不额外分配字符串体。
- `slice_runes`：从头按字节扫描，到`end`即停。
- 不改selfhost（L0-4待同步范围扩为`since` 9–23）。

## 3. 基准（先建基线）

| 编号 | 手写基线 | 原生对照 | 参照 |
| --- | --- | --- | --- |
| strings/15 center_manual → 18 center_native | `rune_count`＋两次`repeat`拼接 | `strings.center` | Python按相同奇偶规则手写`ljust/rjust`（`str.center`奇偶规则不同，不直接用）；Lua `string.rep` |
| strings/16 codepoints_manual → 19 codepoints_native | `bytes.from_text`＋`bytes.at`手写解码；回程手写编码拼接 | `codepoints`＋`from_codepoints` | Python `list(map(ord, s))`、`"".join(map(chr, cs))`；Lua `utf8.codepoint`/`utf8.char` |
| strings/17 slice_runes_manual → 20 slice_runes_native | `bytes.at`数标量定位后`strings.slice` | `slice_runes` | Python切片；Lua `utf8.offset`＋`string.sub` |
| encoding/05 utf8_manual → 06 utf8_native | Hua手写校验与逐字符解码 | `utf8_valid`＋`utf8_decode` | Python `bytes.decode`；Lua `utf8.len`/`utf8.codepoint` |

输入含ASCII、2/3/4字节混合文本，分别报告。

## 4. 验收

1. `center`：奇偶差额、已不短、`width`为0、多字节`fill`、`fill`为空或多于一个标量E4003、负宽度E4003。
2. `codepoints`/`chars`/`from_codepoints`：空串、1–4字节边界（7F/80、7FF/800、FFFF/10000、10FFFF）、D7FF与E000合法、D800/DFFF/110000/负数为Err且下标正确、往返一致；与Python `ord`/`chr`逐项对照；超100万项E4099。
3. `slice_runes`：空区间、全区间、末尾、越界Err、负数Err；与Python切片对照。
4. `utf8_valid`/`utf8_decode`：过长编码（C0 80、E0 80 80、F0 80 80 80）、代理区（ED A0 80）、超范围（F4 90 80 80、F5）、截断、孤立续字节；`utf8_valid`与`bytes.to_text`及Python严格解码三方一致。
5. 三路径一致（AST解释器、源码VM、删除源码后的HUAB）；HUAB23头、降级E6002；静态E3004/E3005；新增ER0行。
6. `ab_bench` core/stdlib与reference_bench对照；受影响CTest；自举C2逐字节不变。

## 5. 需要你决定的点

- **W1 center的奇数差额**：推荐多出的一个放右侧（Rust、Ruby，规则可一句话说清）；备选Python的规则（依赖width奇偶，`"ab".center(5)`与`"abc".center(6)`方向不同）。
- **W2 码点取法**：推荐`codepoints`（`[]int`）与`chars`（`[]string`）都提供：前者算码值，后者`for c in strings.chars(s)`遍历字符最直接；备选只提供`codepoints`，字符靠`from_codepoints([c])`，每次要`unwrap`。
- **W3 码点切片**：推荐`slice_runes`，O(end)，越界Err（与`strings.slice`一致）；备选越界钳制（Python式，但同模块两种规则）。
- **W4 非法码点**：推荐`from_codepoints`/`utf8_encode`返回Err，不替换（与`bytes.to_text`“不替换”一致）；备选替换为U+FFFD（Go `string(rune)`，会静默改数据）。
- **W5 utf8_decode失败形态**：推荐多返回`(-1, 0)`（Result不能包多返回，且扫描循环里只需判断）；备选拆成`utf8_decode -> Result<int,string>`＋`utf8_size(cp)`，多一次调用但错误带说明。
- **W6 模块归属**：推荐文本↔码点放`strings`，`Bytes`↔UTF-8放`encoding`（与现有“编码为Bytes→string”划分一致）；备选新建`std.unicode`，等大小写数据表时一起做（本组则要推迟或之后迁移）。
- **W7 HUAB23**：推荐新格式号23。

**实施前提（不属于接口）：** 工作区里`since` 21/22两批（Set/位工具6个、math 24个）已验收但未提交。推荐先提交这两批，再以其为基线实施本组，否则三批改动混在一起难以单独回退。

## 6. 实施记录（2026-11-07）

- 实现：`src/text_stdlib.cpp`（`center`并入pad分支，新增`codepoints`/`chars`/`from_codepoints`/`slice_runes`）；`src/encoding_stdlib.cpp`（`utf8_valid`复用`text_utf8_error`，`utf8_encode`/`utf8_decode`，导出`append_utf8`）；`src/stdlib.cpp`注册；`src/archive.cpp`接受HUAB23。
- 与提案一致，无语义细化。实施前提“先提交since21/22”未执行：用户定稿时没有要求提交，本组在未提交工作树上实施。
- 结果见[运行记录](../runs/2026-11-07-p1-text-utf8.md)。
