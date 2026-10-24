# HUA-D038第4步后半接口提案：random、安全随机/UUID、SHA-256/HMAC（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，R1–R7全部按推荐（R1 Rng句柄、R2 xoshiro256**+SplitMix64、R3半开区间、R4 std.random与std.crypto分开、R5快照复制并提示、R6单次1 MiB、R7 HUAB13）；先建基线再实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单std.random（候选）、安全随机与标识（候选）、候选encoding/hash/crypto行；现有合同`docs/标准库与内建接口.md`（Bytes/Buffer/List、L0-1回调合同）；实现`src/value.cpp`、`src/sema.cpp`、`src/module.cpp`、`src/archive.cpp`、`src/task_engines.cpp`。格式沿用[math/encoding提案](2026-10-24-math-encoding-p0-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 已有不透明可变句柄类型Buffer/List：独立Value变体、保留类型名、`clone`深复制、只读视图、`==`为E4008、打印`<Buffer:n>`；跨Task按值快照复制 | src/value.cpp:337/430/561、src/sema.cpp:185、src/module.cpp:128、src/task_engines.cpp | 可设种子的PRNG实例最自然的形态是同类新句柄`Rng`；要改的位置有先例可循（约十余处） |
| Hua只有显式种子才可复现；标准库没有任何随机源 | — | PRNG算法、种子展开、区间映射都须**逐位规定**，才能在三路径、HUAB与各平台得到相同序列 |
| 整数`0..n`是半开区间 | 语言设计 | 随机整数区间沿用半开`[lo, hi)` |
| 业务失败用Result（E0-E01）；OS随机源不可用属可预期的环境失败 | 语言设计E0-E01 | 安全随机返回Result |
| 不引入第三方依赖（用户在HUA-D035确认的方向） | 设计决策 | SHA-256/HMAC在本仓库内实现，用FIPS 180-4与RFC 4231向量验收；OS随机源用系统接口（Windows BCryptGenRandom，链接系统库bcrypt；POSIX getrandom/`/dev/urandom`） |
| 本机只有Windows验证环境 | 开发与运行指南 | POSIX随机源路径只能写成“实现、未在本机验证” |

## 1. std.random：可复现的伪随机（`since` 13）

新不透明句柄类型`Rng`（保留类型名，与Buffer同级）。**算法固定为xoshiro256\*\*，种子经SplitMix64展开为4个64位状态**；整数区间用Lemire无偏乘法映射（拒绝采样规则写入合同），float取高53位×2⁻⁵³。同一种子在所有平台、三条执行路径与HUAB上得到逐位相同的序列；将来换算法只能新增函数（如`new_v2`），不改变已有函数的输出。

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `new` | `new(seed int) Rng` | 由种子构造（任意int，含负数与0） |
| `int` | `int(mut r Rng, lo int, hi int) int` | 半开区间`[lo, hi)`均匀整数；`lo>=hi`为E4003；跨度可达2^64−1 |
| `float` | `float(mut r Rng) float` | `[0, 1)`均匀 |
| `bool` | `bool(mut r Rng) bool` | 各1/2 |
| `bytes` | `bytes(mut r Rng, n int) Bytes` | n个伪随机字节（不可用于安全用途）；n<0为E4003，超128 MiB为E4099 |
| `shuffle` | `shuffle(mut r Rng, mut l List<T>) void` | 就地Fisher–Yates（从末尾向前，交换位置由`int(r,0,i+1)`给出，顺序写入合同） |
| `choice` | `choice(mut r Rng, xs []T) Result<T,string>` | 均匀取一项；空输入Err `RANDOM_EMPTY` |
| `sample` | `sample(mut r Rng, xs []T, k int) Result<[]T,string>` | 不放回取k项（部分Fisher–Yates作用于下标副本，结果按抽取顺序）；k>len为Err `RANDOM_RANGE`，k<0为E4003 |

- 句柄语义同Buffer：共享状态、`mut`/可写才能推进（所有取数函数都改变状态，`mutates_first`）；`clone(r)`复制状态得到独立的同序列分叉；`==`为E4008；打印`<Rng>`；不可JSON编码。
- 跨Task：沿用Buffer的按值快照，子Task得到状态副本——**两边会产生相同序列**，文档明确建议每个Task用`random.new(seed + i)`。
- 每次调用一次原生操作、O(1)（`bytes`/`shuffle`/`sample`为O(n)/O(n)/O(k)），无回调。

## 2. std.crypto：安全随机、UUID与哈希（`since` 13）

“crypto”只承诺这里列出的标准算法与OS随机源，不是通用密码库。

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `random_bytes` | `random_bytes(n int) Result<Bytes,string>` | 操作系统CSPRNG；源不可用为Err `CRYPTO_UNAVAILABLE`；n<0为E4003，单次上限1 MiB（E4099） |
| `uuid4` | `uuid4() Result<string,string>` | RFC 9562 UUID v4，小写带连字符（36字符），随机位来自`random_bytes` |
| `sha256` | `sha256(data Bytes) Bytes` | FIPS 180-4 SHA-256，32字节摘要 |
| `hmac_sha256` | `hmac_sha256(key Bytes, message Bytes) Bytes` | RFC 2104/4231 HMAC-SHA256，32字节 |
| `equal` | `equal(a Bytes, b Bytes) bool` | 常量时间比较（长度不同直接false；同长时不提前退出），用于校验MAC |

- 十六进制摘要用已有`encoding.hex_encode(crypto.sha256(b))`；文本先`bytes.from_text`。
- 哈希与HMAC单次输入受Bytes 128 MiB上限；按64字节块线性处理，每1 MiB检查一次取消。
- 不提供：MD5/SHA-1（不安全，若需兼容另议）、流式哈希对象（P1，需要新句柄）、UUID v7（P1，清单已列）。

## 3. 版本、平台与实现位置

- 新函数`since` 13→HUAB13，读取1–13；`Rng`类型名加入HUAB类型校验；不新增opcode。
- 实现：`src/random_stdlib.cpp`（Rng与xoshiro256\*\*）、`src/crypto_stdlib.cpp`（SHA-256、HMAC、OS随机源）；Value新变体`RngValue{shared_ptr<RngState>, writable}`，按Buffer的先例接入类型名、只读、clone、打印、相等、Task快照、sema保留名与类型列表。
- 平台：Windows用`BCryptGenRandom`（系统库，非第三方依赖）；POSIX用`getrandom`，不可用时读`/dev/urandom`——**POSIX路径实现但本机未验证**，文档如实标注。
- 不改selfhost（自举暂停，L0-4登记）。

## 4. 基准（实施前先测手写基线；`perf/stdlib/random`、`perf/stdlib/crypto`）

- random/01 int_float_manual（Hua手写LCG＋取模）→ native `int`/`float`；参照Python `random.Random(seed)`、Lua `math.random`（参照语言算法不同，只比速度不比序列）。
- random/02 shuffle_sample_manual（手写LCG＋`list.get/set`的Fisher–Yates）→ native `shuffle`/`sample`。
- crypto/01 sha256_bulk（1 MiB数据×若干轮，无手写对照）；参照Python `hashlib`、Lua纯脚本实现。
- crypto/02 hmac_small（大量短消息HMAC）；参照Python `hmac`。
- crypto/03 uuid4（10万个）；参照Python `uuid.uuid4()`。
- 每步`ab_bench`（core与stdlib既有）无退化，可疑项交替计时复核。

## 5. 验收

1. 可复现：固定种子的前若干输出与独立参考实现（测试脚本里用Python按同一算法计算）逐位一致；三路径与HUAB一致；`clone`分叉后两边同序列。
2. 区间：`int`的边界（跨度1、跨度2^63以上、负区间）、无偏映射的拒绝分支被覆盖；`lo>=hi` E4003；`shuffle`为排列且固定种子结果固定；`choice/sample`空输入与k越界Err。
3. 安全随机：长度正确、两次调用不同、`uuid4`格式（版本位4、变体位10xx）；源失败路径通过注入测试或说明。
4. 哈希：FIPS 180-4向量（""、"abc"、448位与896位消息、百万个"a"）、RFC 4231 HMAC测试用例1–7；`equal`长度不同/相同/不同内容。
5. 句柄：只读`Rng`推进为E3003/E4007，`==`为E4008，跨Task快照行为；HUAB13往返、旧程序格式不变、降级头E6002；`er0_inventory`新增ER0-X行。
6. 基准表写入路线文档；CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变。

## 6. 需要你决定的点

- **R1 PRNG形态**：推荐新句柄`Rng`（显式种子、可复现、可分叉）；备选模块级隐式状态（`random.seed(n)`后直接`random.int(lo,hi)`，Python模块函数式写法，但多Task/库代码间共享隐式状态，复现性差）。
- **R2 算法**：推荐xoshiro256\*\*＋SplitMix64种子展开（快、规格公开、纯整数运算易逐位复现）；备选PCG64（需128位乘法）或SplitMix64单独使用（更简单但统计质量较弱）。
- **R3 整数区间**：推荐半开`[lo, hi)`（与`0..n`一致）；备选闭区间`[lo, hi]`（Python `randint`风格）。
- **R4 模块划分**：推荐`std.random`（可复现PRNG）与`std.crypto`（安全随机、UUID、SHA-256、HMAC、常量时间比较）分开，避免把伪随机误用于安全用途；备选全部放`std.random`，哈希另放`std.hash`。
- **R5 跨Task**：推荐沿用Buffer的快照复制并在文档提示“两边序列相同”；备选禁止`Rng`跨Task（E4104，类似File）。
- **R6 安全随机上限**：推荐`random_bytes`单次≤1 MiB；备选沿用Bytes的128 MiB。
- **R7 HUAB13**：推荐新格式号13。

## 7. 实施记录（2026-10-24）

- 全部按定稿实现（random 8个、crypto 5个，HUAB13）；`RngValue`为Value变体末尾新增项（不改变既有变体下标）。
- 与提案差异：`random.*`在`invoke_standard`入口早分派并自行检查实参种类与写能力（通用参数检查使`int/float`慢于手写LCG：0.305→0.203秒）；`uuid4`不做随机字节池。
- 测试`tests/random_crypto_tests.py`以独立Python模型验证可复现序列；Task快照为只读副本已手测（未写入自动测试）。
- 结果表见docs/标准库设计与实现路线.md“第4步后半random/crypto结果”。
