# HUA-D038 P0缺口接口提案：alg unique/chunk/product（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，U1–U7全部按推荐（U1全局去重保留首次出现顺序、U2直接返回值、U3按`==`合并±0、U4哈希表、U5任意元素类型＋独立只读副本、U6任一中间结果溢出即Err、U7 HUAB19）；先建基线再实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单std.alg行（P0缺口`unique`、`chunk`、`product`，这是P0唯一剩余缺口；同行P1的`partition`/`zip`/`windows`与P2的`group_by`/排列组合不在本组）；[接口参考std.alg](../../../../docs/标准库与内建接口.md)；`src/algorithm_stdlib.cpp`（非回调alg实现）；`src/sema.cpp`（alg元素类型检查）。格式沿用[test/log提案](2026-10-24-test-log-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 非回调alg只接受元素为int/float/string的数组或Slice（静态E3004、运行时E4003）；`sum`另排除string | sema.cpp、algorithm_stdlib.cpp | `unique`/`product`沿用同一限制；`chunk`不比较元素，可放宽到任意T（见U5） |
| `sorted`/`reverse`返回`Result<[]T,string>`，但成功以外的情况（超限）其实是E4099诊断，Result从不为Err；L0-1回调组（`map`/`filter`等）改为直接返回值 | 接口文档std.alg | 新函数的返回形态需要选一种约定（见U2） |
| `sum`返回`Result<T,string>`，int溢出或float非有限为Err `ALG_OVERFLOW`，空输入为0/0.0 | algorithm_stdlib.cpp | `product`与`sum`对称（见U6） |
| float只允许有限值（E0-N04），不存在NaN；`==`下±0相等 | 语言设计 | `unique`的float相等无NaN特例，只需把−0.0与0.0视为同一值 |
| 嵌套Slice`[][]T`可用（`list.snapshot`得到`[][]int`） | 已验证 | `chunk`可以返回`[][]T` |
| 限额：最多1,000,000项、32,000,000内部工作单位、复制字符串总量16 MiB（E4099），协作取消E4101 | 接口文档std.alg | 三个函数共用 |

## 1. 接口（`since` 19）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `unique` | `unique(xs []T) []T` | 去重，**保留每个值第一次出现的位置与顺序**；T为int/float/string；float按`==`（−0.0与0.0为同一值，保留先出现者的原样） |
| `chunk` | `chunk(xs []T, size int) [][]T` | 按顺序切成每块`size`项，最后一块可以更短；空输入得到空结果；`size <= 0`为E4003；T为任意元素类型 |
| `product` | `product(xs []T) Result<T,string>` | 从左到右连乘；T为int/float；空输入为1/1.0；int中间结果溢出或float结果非有限为Err `ALG_OVERFLOW: ...` |

- 参数可以是数组或Slice（与现有alg一致）；结果是只读的独立Slice（`chunk`的外层与每个内层都是），需要写时`clone`。
- `chunk`的块数为⌈n/size⌉；`size`大于n时得到一块（整个输入）。

例：

```hua
import std.alg as alg
fn main() {
    print(alg.unique([3, 1, 3, 2, 1]))          # [3, 1, 2]
    print(alg.chunk(["a", "b", "c", "d", "e"], 2)) # [[a, b], [c, d], [e]]
    print(unwrap(alg.product([2, 3, 7])))         # 42
}
```

## 2. 实现与性能

- 位置：`src/algorithm_stdlib.cpp`，与`sum`/`sorted`同一入口；sema按第一参数推出`[]T`、`[][]T`、`Result<T,string>`。
- `unique`：开放寻址哈希表，按输入长度预留容量，期望O(n)；int按值、float按位（先把−0.0规范成0.0）、string按内容哈希，表里只存下标不复制字符串；输出按首次出现顺序复制。
- `chunk`：一次预留⌈n/size⌉个块，每块按块长预留后复制元素；无比较。
- `product`：int用带溢出检查的乘法（`__builtin_mul_overflow`），float每步检查有限。
- 不改selfhost（自举暂停，L0-4登记）。

## 3. 基准（先建基线）

| 编号 | 手写基线 | 原生对照 | 参照 |
| --- | --- | --- | --- |
| alg/11 unique_manual → 14 unique_native | `map[T]bool`记录已见＋`list.append`＋`snapshot`（20万项、约一半重复，int与string各一轮） | `alg.unique` | Python `list(dict.fromkeys(xs))`；Lua表记录已见 |
| alg/12 chunk_manual → 15 chunk_native | 下标循环逐块`list.append`后`snapshot`（20万项、块长16） | `alg.chunk` | Python列表切片推导；Lua双层循环 |
| alg/13 product_manual → 16 product_native | `for`循环连乘（int与float，含Result形式的溢出检查） | `alg.product` | Python `math.prod`；Lua循环 |

## 4. 验收

1. `unique`：首次出现顺序、int/float/string、−0.0与0.0合并、空输入、全相同、无重复、数组参数；struct/bool元素静态E3004。
2. `chunk`：整除与不整除、`size`大于n、`size`为1、空输入、struct元素与嵌套Slice元素、结果只读（写入报错）、`size <= 0`为E4003、size非int静态E3004。
3. `product`：int/float、空输入1/1.0、int溢出Err（含正负边界）、float溢出Err、含0、string元素静态E3004。
4. 三路径一致（AST解释器、源码VM、删除源码后的HUAB）；HUAB19往返、降级头E6002；限额E4099；`er0_inventory`新增ER0-AD01–AD03。
5. `ab_bench` core/stdlib无退化，reference_bench对照CPython/Lua；CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变。

## 5. 需要你决定的点

- **U1 unique的含义**：推荐A——全局去重、保留首次出现顺序（Kotlin `distinct`、lodash `uniq`、Python `dict.fromkeys`）；备选B——只去掉相邻重复（Rust `dedup`、Go `slices.Compact`、C++ `std::unique`，留给P1的`list.dedup`）；备选C——排序后去重（numpy `unique`，等于`unique(sorted(xs))`，可由组合得到）。
- **U2 返回形态**：推荐`unique`/`chunk`直接返回`[]T`/`[][]T`（与L0-1回调组一致；超限本来就是E4099诊断，Result永远不会是Err）；备选与`sorted`/`reverse`对称返回`Result<…,string>`（需要`unwrap`，没有实际的Err路径）。
- **U3 unique的float相等**：推荐按`==`把−0.0与0.0视为同一值，保留先出现者（与`==`、`alg.contains`一致）；备选按位区分（两者都保留）。
- **U4 unique的算法**：推荐哈希表，期望O(n)，保持首次出现顺序；备选基于排序O(n log n)（最坏情况有保证但更慢，且需要额外的下标排序才能保持顺序）。
- **U5 chunk的元素类型与结果**：推荐任意元素类型（不比较元素），返回独立的只读副本；备选A——同其他alg只收int/float/string；备选B——内层返回共享输入存储的只读视图（少复制，但输入后续被修改时块内容随之变化，与“独立Slice”约定不一致）。
- **U6 product的溢出规则**：推荐从左到右连乘、任一中间结果溢出即Err（与`sum`、Rust `checked`语义一致，结果不依赖“后面有没有0”的预读）；备选遇到0立即返回0（`[大数, 大数, 0]`得0而不是Err）。
- **U7 HUAB19**：推荐新格式号19。

## 6. 实施记录（2026-10-24）

- 实现：`src/algorithm_stdlib.cpp`（`unique`哈希表去重、`chunk`、`product`）；`src/stdlib.cpp`注册与`since` 19提前分派；`src/binary_stdlib.cpp`签名；`src/sema.cpp`元素类型检查；`src/archive.cpp`接受HUAB19。
- 与提案一致，无语义细化；`unique`/`chunk`超限与其他alg一样为E4099诊断。
- 验证：`tests/alg_p0_tests.py`（41项）；ER0-AD01–AD03。性能：原生比手写快5.7–7.9倍；A/B中strings组2–4%的一致变慢按文件二分定位到三条注册表项的位置，换位置后与HEAD持平（布局效应）。结果见[标准库路线](../../../../docs/标准库设计与实现路线.md)“P0缺口alg unique/chunk/product结果”。
