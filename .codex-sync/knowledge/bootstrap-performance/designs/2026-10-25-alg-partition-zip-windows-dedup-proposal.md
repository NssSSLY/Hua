# HUA-D038 P1接口提案：alg partition/zip_with/windows与list dedup（2026-10-25用户定稿）

状态：**2026-10-25用户定稿**，V1–V7全部按推荐（V1多返回`([]T, []T)`、V2 `zip_with`带回调、V3按较短者截断、V4窗口共享一份只读副本、V5只提供步长1、V6就地返回删除个数并按`==`比较、V7 HUAB20）；先建基线再实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单std.alg行P1（`sort_by`/`min_by`/`max_by`/`any`/`all`已由L0-1回调组完成，剩`partition`/`zip`/`windows`）与std.list行P1（`dedup`）；做完后P1“alg”一组由部分完成变为完成。[接口参考std.alg/std.list](../../../../docs/标准库与内建接口.md)；`src/callback_stdlib.cpp`（回调alg/list）；`src/algorithm_stdlib.cpp`（非回调alg）；`src/collections_stdlib.cpp`（list，`same_scalar`即语言`==`）；`src/sema.cpp`（回调签名检查）。格式沿用[alg unique/chunk/product提案](2026-10-24-alg-unique-chunk-product-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 语言没有通用Tuple；多返回`(T1,T2)`可用，标准函数已有多返回先例`time.utc_parts`（注册结果`multi<…>`） | 语言设计“多返回”、stdlib.cpp | `partition`可以多返回两段；`zip`无法返回“对”的切片（见V2） |
| 回调alg（`map`/`filter`/`any`…）要求回调为带注解、非mut参数、单一非async结果的Hua函数；谓词须返回bool；每次回调计1工作单位，上限3200万（E4099），协作取消E4101 | sema.cpp、callback_stdlib.cpp | `partition`/`zip_with`沿用同一检查与限额 |
| `list.retain`就地修改、返回删除个数`int`，修改期间回调改动List为E4003 | callback_stdlib.cpp | `list.dedup`对称：就地、返回删除个数 |
| `list.index_of`/`contains`按语言`==`比较（int/float/string/bool快路径，其余走结构相等） | collections_stdlib.cpp `same_scalar` | `dedup`可以对任意元素类型按`==`去相邻重复 |
| `chunk`返回独立只读副本（外层与每块），总复制量为n | algorithm_stdlib.cpp | `windows`若同样逐窗复制，总量为(n−size+1)×size，可能远大于n（见V4） |
| Slice值带起点与长度（`SliceValue{storage,start,length,writable,element}`） | value | 多个只读窗口可以共享同一份存储 |

## 1. 接口（`since` 20）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `alg.partition` | `partition(xs []T, pred fn(T) bool) ([]T, []T)` | 多返回：先是`pred`为true的元素，再是其余元素；两段都保持原顺序（稳定）；每个元素恰好调用一次`pred`，从左到右；T为任意元素类型 |
| `alg.zip_with` | `zip_with(a []A, b []B, f fn(A, B) R) []R` | 按下标两两调用`f(a[i], b[i])`，结果长度为`min(len(a), len(b))`；A、B、R可为任意类型 |
| `alg.windows` | `windows(xs []T, size int) [][]T` | 长度为`size`、步长为1的全部连续窗口，共`n−size+1`个；`size > n`时为空结果；`size <= 0`为E4003；T为任意元素类型 |
| `list.dedup` | `dedup(l List<T>) int` | 就地删除**相邻**的`==`相等项，只保留每段连续相等的第一个；返回删除的个数；需要可写List |

- `partition`/`zip_with`/`windows`的参数可以是数组或Slice；结果是只读Slice，需要写时`clone`。
- `partition`用法：`let evens, odds = alg.partition(xs, is_even)`。
- 全局去重已有`alg.unique`；`list.dedup`与Rust `Vec::dedup`、Go `slices.Compact`、C++ `std::unique`一致，先`list.sort`再`dedup`即得到排序去重。

例：

```hua
import std.alg as alg
import std.list as list
fn is_even(x int) bool { return x % 2 == 0 }
fn add(x int, y int) int { return x + y }
fn main() {
    let evens, odds = alg.partition([1, 2, 3, 4, 5], is_even)
    print(evens, odds)                              # [2, 4] [1, 3, 5]
    print(alg.zip_with([1, 2, 3], [10, 20], add))   # [11, 22]
    print(alg.windows([1, 2, 3, 4], 3))             # [[1, 2, 3], [2, 3, 4]]
    var l = list.from_slice([1, 1, 2, 2, 2, 1, 3, 3])
    print(list.dedup(l), list.snapshot(l))          # 4 [1, 2, 1, 3]
}
```

## 2. 实现与性能

- `partition`、`zip_with`：`src/callback_stdlib.cpp`的回调alg入口；sema推出`multi<[]T,[]T>`与`[]R`，回调检查沿用`filter`（谓词bool）与`sorted_by`（二元，参数分别检查A、B）。
- `windows`：`src/algorithm_stdlib.cpp`，与`chunk`同一入口。推荐实现（V4-A）：把输入复制一次到一份只读存储，每个窗口是指向它的只读Slice（起点i、长度size）；复制量与内存都是O(n)，窗口之间共享但都不可写，输入之后被修改也不影响结果。
- `dedup`：`src/collections_stdlib.cpp`，单次扫描、按`same_scalar`比较前一个保留项，原地压缩后截断；O(n)，不分配新存储，修改后`revision`加1。
- 签名在`src/binary_stdlib.cpp`；`src/archive.cpp`接受HUAB20。不改selfhost（自举暂停，L0-4登记）。

## 3. 基准（先建基线）

| 编号 | 手写基线 | 原生对照 | 参照 |
| --- | --- | --- | --- |
| alg/17 partition_manual → 20 partition_native | `for`循环调用谓词、两个List分别`append`后`snapshot`（2万项×10） | `alg.partition` | Python两个列表推导；Lua循环 |
| alg/18 zip_manual → 21 zip_native | 下标循环调用二元函数、`list.append`后`snapshot`（2万项×10） | `alg.zip_with` | Python `[f(x, y) for x, y in zip(a, b)]`；Lua循环 |
| alg/19 windows_manual → 22 windows_native | 下标循环`list.slice`或逐窗`append`（2万项、窗长8，×10，读取每窗首尾求和） | `alg.windows` | Python切片推导；Lua双层循环 |
| list/09 dedup_manual → 10 dedup_native | 逐项与前一个比较、新List `append`（int与string各2万项×10，约一半相邻重复） | `list.dedup` | Python `itertools.groupby`取键；Lua循环 |

## 4. 验收

1. `partition`：全真、全假、空输入、稳定顺序、struct元素、调用次数与顺序（带副作用的回调计数）、回调非bool静态E3004、回调参数类型不符E3004、多返回绑定数量不符E3006。
2. `zip_with`：等长、不等长两种方向、空输入、A/B/R各不相同（如int与string得到string）、回调参数个数不符E3005。
3. `windows`：size=1、size=n、size>n（空）、空输入、struct与嵌套Slice元素、结果只读E3003、`clone`后可写、输入是可写Slice时之后修改不影响结果、`size <= 0`为E4003、size非int静态E3004。
4. `dedup`：无重复、全相同、首尾成段、非相邻重复保留、float ±0视为相等、string、struct元素、空List、只读List参数E3003、返回值与`len`一致。
5. 三路径一致（AST解释器、源码VM、删除源码后的HUAB）；HUAB20往返、降级头E6002；限额E4099；`er0_inventory`新增ER0-AE01–AE04。
6. `ab_bench` core/stdlib无退化，reference_bench对照CPython/Lua；CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变。

## 5. 需要你决定的点

- **V1 partition的返回形态**：推荐A——多返回`([]T, []T)`（与`utc_parts`同一机制；Kotlin `partition`返回Pair、Rust `partition`返回二元组）；备选B——只返回匹配的一段（等于`filter`，没有意义）；备选C——返回`[][]T`两项（取用要下标，类型上看不出哪段是哪段）。
- **V2 zip的形态**：推荐A——`zip_with(a, b, f)`，由回调决定把一对变成什么（Haskell `zipWith`、Kotlin `zip(other) { a, b -> }`、Swift `zip`后`map`），把`zip`名字留给将来有Tuple时；备选B——叫`zip`但同样带回调；备选C——`zip(a, b) ([]A, []B)`多返回截成等长（只做截断，几乎没有用处）；备选D——标准库定义`Pair` struct返回`[]Pair<A,B>`（需要先有标准泛型struct，工作量大，暂不推荐）。
- **V3 zip_with长度不等**：推荐按较短者截断（Python `zip`、Kotlin、Rust、Haskell默认行为）；备选长度不等为E4003（Python `zip(strict=True)`，能抓到错位但不符合多数语言习惯）。
- **V4 windows的结果存储**：推荐A——复制一次输入，各窗口是共享这份只读存储的只读Slice（O(n)复制和内存；输入之后被修改不影响结果；与`chunk`“结果独立于输入、只读”的约定一致）；备选B——每个窗口单独复制（与`chunk`实现一致，但n=2万、size=1000时要复制约2000万项，容易触发E4099）；备选C——窗口直接指向输入存储（零复制，但输入是可写Slice时结果会随之变化）。
- **V5 windows的步长**：推荐只提供步长1（Rust `windows`、Kotlin `windowed`默认）；步长等于size已由`chunk`覆盖，其他步长等有需要再加`windows_step`；备选现在就加第三参数`step`。
- **V6 dedup的返回与比较**：推荐就地修改、返回删除个数`int`、按语言`==`比较任意元素类型（与`retain`返回值、`index_of`比较规则一致）；备选A——返回`void`（Rust `dedup`）；备选B——不改原List，返回新List（与其他list就地方法不一致）；`dedup_by`按回调比较留到P2。
- **V7 HUAB20**：推荐新格式号20。

## 6. 实施记录（2026-10-25）

- 实现：`src/callback_stdlib.cpp`（`partition`、`zip_with`）；`src/algorithm_stdlib.cpp`（`windows`，窗口共享一份只读副本）；`src/collections_stdlib.cpp`（`list.dedup`）；`src/stdlib.cpp`注册（四条放在表尾）；`src/binary_stdlib.cpp`签名；`src/sema.cpp`回调与元素类型检查；`src/archive.cpp`接受HUAB20。
- 与提案一致，无语义细化；`dedup`先比较后压缩，比较出错时List不变。
- 验证：`tests/alg_p1_tests.py`（42项）；ER0-AE01–AE04。性能：原生比手写快2.7–11.6倍；A/B中maps/02稳定慢约15%，二分到std.alg存根数26→27，字节码相同、从HUAB运行无差别、`main`开头多分配少量数据即持平，判定为编译后堆状态的布局效应。CTest （本机tests/build配置95项）并行首轮11项失败，逐项重跑后85项通过（compiler_entry重跑通过）；余下10项都是既有问题：HUA-D037的4项断言（archives_extensions、selfhost_ast_pool E4099、selfhost_frontend_extended E8201@14:11、selfhost_lowering capability_passes上限），以及HUA-D036自举入口超时6项（selfhost_parser、selfhost_types、selfhost_bytecode、selfhost_archive、type_contract、capability_provenance；超时的types_main命令base 20.1秒、new 20.2秒，base同样超出约9秒的自带时限）；C2逐字节不变。结果见[标准库路线](../../../../docs/标准库设计与实现路线.md)“P1 alg partition/zip_with/windows与list dedup结果”。
