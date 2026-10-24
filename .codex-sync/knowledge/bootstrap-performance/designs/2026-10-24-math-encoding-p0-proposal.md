# HUA-D038第4步接口提案：math P0与encoding P0（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，G1–G7全部按推荐（G1不提供is_nan/is_inf、G2不新增pow、G3整数函数返回普通int、G4零参函数常数、G5单一std.encoding、G6标准版严格URL版填充可选、G7 HUAB12）；先建基线再实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单的std.math行与std.encoding（候选）行；现有合同`docs/标准库与内建接口.md`“std.math”“数学：std.math，12接口”“std.bytes”，语言规则E0-N01/N04、E0-E01；实现`src/value.cpp`（`**`）、`src/algorithm_stdlib.cpp`（math）、`src/stdlib.cpp`、`src/sema.cpp`。格式沿用[切片2提案](2026-10-24-slice2-map-list-set-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 默认float恒为有限值：运算产生溢出/NaN/Infinity即E4002（E0-N04），`enforce_type`拒绝非有限float | 语言设计E0-N04；src/value.cpp | **`is_nan`/`is_inf`对Hua的float恒为false**，没有意义（见G1） |
| `int ** int`已是带检查的整数幂：溢出E4002、负指数E4005；`float ** float`按有限性检查 | src/value.cpp:309/329 | 清单里的“整数`pow`（溢出报错）”**已由运算符覆盖**；浮点`pow`同理（见G2） |
| 现有std.math 12个函数只收默认float、全部返回`Result<float,string>`（MATH_DOMAIN/MATH_OVERFLOW）；运行时与sema都对整个math模块强制float实参 | src/stdlib.cpp:275、src/sema.cpp:620 | 新的整数函数要豁免这条检查；返回形态要定规则（见G3） |
| 标准库只导出函数，没有模块常量机制（`math.pi`这种成员值不存在） | src/module.cpp、标准函数表 | 常量要么做成零参函数，要么新增常量导出机制（见G4） |
| 已有全局内建`sqrt/abs/min/max/clamp` | 接口文档“无需导入的公开函数” | 不重复提供 |
| Bytes不可变、上限128 MiB；string上限16 MiB且恒为合法UTF-8 | src/binary_stdlib.cpp；语言设计string-utf8-invariant | 编码输出string须≤16 MiB；解码输入是string，输出Bytes |
| 业务失败用Result；越界/溢出/类型/预算是诊断（E0-E01） | 语言设计E0-E01 | 解码非法输入是可预期的业务失败→Result；整数溢出→E4002诊断 |

## 1. std.math增量（`since` 12）

### 1.1 整数函数（参数与结果为int；溢出E4002，与整数运算符一致）

| 函数 | 签名 | 语义 | 复杂度 |
| --- | --- | --- | --- |
| `gcd` | `gcd(a int, b int) int` | 最大公约数，结果≥0；`gcd(0,0)=0`；结果为2^63（如`gcd(INT64_MIN,0)`）时E4002 | O(log min) 二进制GCD |
| `lcm` | `lcm(a int, b int) int` | 最小公倍数，结果≥0；任一为0得0；溢出E4002 | O(log) |
| `isqrt` | `isqrt(n int) int` | ⌊√n⌋；n<0为E4003 | O(1)（浮点初值＋整数校正，精确） |

### 1.2 浮点函数（默认float，返回`Result<float,string>`，与现有12个一致）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `asin` / `acos` | `(x float) Result<float,string>` | |x|>1为MATH_DOMAIN |
| `atan` | `(x float) Result<float,string>` | 结果(−π/2, π/2) |
| `atan2` | `(y float, x float) Result<float,string>` | 象限正确；`atan2(0,0)`=0（沿用C库，±0规则写明） |
| `cbrt` | `(x float) Result<float,string>` | 实数立方根，负数可用 |
| `fmod` | `(x float, y float) Result<float,string>` | 截断余数，符号随x；y为0时MATH_DOMAIN（与`%`的整数/向下取整语义不同，文档对照写明） |

### 1.3 常数

推荐（G4-A）：零参函数`math.pi() float`、`math.e() float`、`math.tau() float`，不包Result（无失败）。热循环里先`let pi = math.pi()`。

## 2. std.encoding（新模块，`since` 12）

输入/输出类型按“编码=Bytes→string，解码=string→Bytes”。文本先`bytes.from_text(s)`再编码（显式，无隐式转换）。

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `hex_encode` | `hex_encode(b Bytes) string` | 小写十六进制，每字节两字符 |
| `hex_decode` | `hex_decode(s string) Result<Bytes,string>` | 接受大小写；奇数长度或非十六进制字符Err `ENC_SYNTAX: ... at byte N` |
| `base64_encode` | `base64_encode(b Bytes) string` | RFC 4648标准字母表，带`=`填充 |
| `base64_decode` | `base64_decode(s string) Result<Bytes,string>` | 严格：必须是标准字母表、长度为4的倍数、填充正确、不接受空白与非零尾随位；否则`ENC_SYNTAX` |
| `base64url_encode` | `base64url_encode(b Bytes) string` | URL安全字母表（`-`/`_`），**不带填充**（JWT/URL常用） |
| `base64url_decode` | `base64url_decode(s string) Result<Bytes,string>` | URL安全字母表；有无填充都接受，有填充时必须正确 |

- 上限：编码结果超过16 MiB（hex输入>8 MiB，base64输入约>12 MiB）为E4099（分配前判定）；解码结果受Bytes 128 MiB上限（实际受输入16 MiB约束）。
- 性能：一次扫描、按精确长度一次分配、查表编码/解码；不经过中间`[]int`或逐字节Value。
- 错误：只有非法输入是Err（`ENC_SYNTAX`前缀＋首个非法位置）；类型错误E3004/E4003。

## 3. 不纳入P0的清单项与原因（写入路线文档）

- `is_nan`/`is_inf`：Hua的float不能持有NaN/Infinity（E0-N04），恒为false；若将来开放非有限值或f32/f64的IEEE模式再加（G1）。
- 整数`pow`：`**`已提供带溢出检查的整数幂；`checked_pow`（返回Result）属P1的checked/wrapping/saturating组（G2）。
- 浮点`pow`：`**`已提供；如需Result形态的`math.pow`见G2备选。

## 4. 版本、三路径、实现位置

- 新函数`since` 12→HUAB12，读取1–12；不新增opcode。
- 实现：math增量放`src/algorithm_stdlib.cpp`的math分支（整数函数绕过“只收默认float”检查：运行时与sema都按`since>=12`的函数名豁免）；encoding新文件`src/encoding_stdlib.cpp`。三路径经同一`invoke_standard`。
- 不改selfhost（自举暂停，L0-4登记）。

## 5. 基准（实施前先测手写基线；`perf/stdlib/math`、`perf/stdlib/encoding`，各含`.hua/.py/.lua/.expect`）

- math/01 gcd_lcm_manual（Hua手写欧几里得）→ native；参照Python `math.gcd/lcm`、Lua手写。
- math/02 isqrt_manual（Hua手写牛顿迭代）→ native；参照Python `math.isqrt`。
- math/03 trig_native（atan2/asin/acos/cbrt/fmod混合，无手写对照，只对参照语言）。
- encoding/01 hex_manual（`bytes.to_ints`＋查表拼接）→ native；参照Python `bytes.hex()/bytes.fromhex()`、Lua `string.format("%02x")`。
- encoding/02 base64_manual（Hua手写编码/解码）→ native；参照Python `base64`模块、Lua手写实现。
- 每步`ab_bench`（core与stdlib既有）无退化，可疑项交替计时复核。

## 6. 验收

1. 三路径一致：gcd/lcm负数与0、INT64_MIN边界E4002、isqrt的完全平方/大数（2^62附近）/负数E4003；反三角边界±1、MATH_DOMAIN；atan2四象限与±0；fmod符号与y=0；hex/base64的RFC 4648测试向量（""、"f"、"fo"、"foo"、"foob"、"fooba"、"foobar"）、非法字符/长度/填充/尾随位、URL版有无填充；编码超16 MiB E4099。
2. 静态：math整数函数收float为E3004、浮点函数收int仍E3004（保持“显式默认float”规则）；encoding参数类型E3004。
3. HUAB12往返、旧程序格式不变、降级头E6002；`er0_inventory`新增ER0-W行。
4. 基准表与参考语言倍数写入路线文档；CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变。
5. 文档：接口（签名、ER0-W、计数）、运行时（HUAB12）、路线（含§3不纳入原因）、设计决策、概览、百科计数。

## 7. 需要你决定的点

- **G1 is_nan/is_inf**：推荐不提供（Hua float恒有限，恒为false）；备选按清单提供（恒返回false，仅为对齐名称）。
- **G2 pow**：推荐不新增（`**`已是带检查的整数/浮点幂）；备选新增`math.pow(x float, y float) Result<float,string>`（与math其余函数一致的可恢复形态），整数checked版留P1。
- **G3 整数函数返回形态**：推荐`gcd/lcm/isqrt`返回普通int、溢出E4002/负数E4003（与整数运算符一致，E0-E01把溢出归诊断）；备选返回`Result<int,string>`（与现有math浮点函数的Result形态一致，但每次调用多一次Result包装/解包）。
- **G4 常数**：推荐A零参函数`math.pi()/e()/tau()`（无新机制）；备选B新增标准模块常量导出`math.pi`（更像Python/Go，但要改模块导出、sema成员解析、lowering与HUAB常量引用，范围明显更大）。
- **G5 encoding模块形态**：推荐单一`std.encoding`、函数名带格式前缀；备选按格式分模块`std.hex`/`std.base64`（Go风格，模块数+2）。
- **G6 base64解码严格度**：推荐标准版严格（必须填充、无空白），URL版填充可选；备选两者都宽松（忽略空白、填充可选）。
- **G7 HUAB12**：推荐新格式号12（与前几步一致）。

## 8. 实施记录（2026-10-24）

- 全部按定稿实现：math 12个（`src/algorithm_stdlib.cpp`的`math_p0`：二进制GCD、lcm先除后乘并检查、isqrt浮点初值加128位精确校正）、std.encoding 6个（`src/encoding_stdlib.cpp`，查表、一次分配、严格RFC 4648）；HUAB12。
- 与提案差异：无。HUAB降级检查沿用逐条标准引用的E6002。
- 结果表见docs/标准库设计与实现路线.md“第4步math/encoding结果”：gcd/lcm 3.3x、isqrt 2.3x、hex 25x、base64 21x（后两者与CPython持平）。
