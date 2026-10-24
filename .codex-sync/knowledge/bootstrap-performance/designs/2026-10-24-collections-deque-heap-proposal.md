# HUA-D038第5步第一组接口提案：Deque与Heap（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，D1–D7全部按推荐（D1新句柄Deque<T>、D2 heap作用于List、D3每次比较调键函数、D4不承诺同优先级顺序、D5 std.deque与std.heap、D6 HUAB14、D7按推荐顺序）；先建基线再实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)“建议执行顺序”第5条、对齐清单std.collections行与S1c容器行（“T/K范围、空pop、稳定同优先级、容量/复杂度/mut”，验收“去重、BFS、top-k”）；现有合同`docs/标准库与内建接口.md`（std.list、L0-1回调合同、std.random的句柄接入）。格式沿用[random/crypto提案](2026-10-24-random-crypto-proposal.md)。

## 0. 为什么第5步先做这一组

第5步五组（collections、Duration与UTC日期、os exit/平台、fs walk/copy、test/log）按“性能优先”排序：collections直接决定BFS/调度/top-k这类算法负载的速度——今天只能用`List`加下标模拟队列、用`list.get/set`手写堆，每次出入队都是多次标准调用；其余四组主要是功能补齐，对热点影响小。建议顺序：collections → Duration/UTC → os exit/平台 → fs walk/copy → test/log（顺序可由你调整）。

## 1. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| `List<T>`是`std::vector<Value>`：尾部O(1)，头部插删O(n)；`list.reserve`的合同返回“是否扩容”，依赖vector容量 | src/binary_stdlib.cpp、接口文档std.list | 把List换成`std::deque`可得O(1)头部操作，但`std::deque`没有capacity，会改变`reserve`的已冻结合同（见D1备选） |
| 泛型容器类型`List<T>`在sema/值/HUAB类型校验/Task快照/打印/相等中约48处接入 | grep `List<`/`ListValue` | 新增一个泛型句柄`Deque<T>`的工作量约等于再接一遍List；`std.random.Rng`（非泛型）的接入清单可作模板 |
| 没有Tuple，struct不可比较大小 | 语言设计 | “优先级＋负载”的堆不能直接用自然序；需要键回调（L0-1已有）或新类型 |
| L0-1回调已可用，单次回调约等于一次Hua函数调用 | L0-1回调合同 | 按键的堆每次push/pop需O(log n)次键回调；或在入堆时只算一次键（见D3） |
| ListData有修订计数（E5） | src/value.hpp | 堆函数若作用于List，回调改动同一List可同样检测 |

## 2. Deque（推荐D1-A：新泛型句柄`Deque<T>`，模块`std.deque`，`since` 14）

底层`std::deque<Value>`；能力规则同List（共享句柄、`mut`/可写才能修改、`clone`深复制、`==`按元素比较、打印`<Deque:n>`、跨Task只读快照、上限100万项）。

| 函数 | 签名 | 语义 | 复杂度 |
| --- | --- | --- | --- |
| `from_slice` | `from_slice(xs []T) Deque<T>` | 由类型已定的Slice/数组深复制（空队列同List：`let e []int = []`） | O(n) |
| `len` | `len(d Deque<T>) int` | 元素数 | O(1) |
| `push_back` / `push_front` | `(mut d Deque<T>, x T) Result<int,string>` | 两端加入，成功值为新长度；满100万Err `deque item limit exceeded` | O(1)均摊 |
| `pop_back` / `pop_front` | `(mut d Deque<T>) Result<T,string>` | 两端取出；空为Err `DEQUE_EMPTY` | O(1) |
| `front` / `back` | `(d Deque<T>) Result<T,string>` | 只看不取；空为Err `DEQUE_EMPTY` | O(1) |
| `get` | `get(d Deque<T>, i int) Result<T,string>` | 下标读取；越界Err | O(1) |
| `clear` | `clear(mut d Deque<T>) void` | 清空 | O(n) |
| `snapshot` | `snapshot(d Deque<T>) []T` | 从头到尾的只读Slice副本（遍历用） | O(n) |

读取返回值的共享嵌套容器只读（同`list.get/pop`）。

## 3. Heap（推荐D2-A：作用于`List<T>`的函数，模块`std.heap`，`since` 14；不新增类型）

最小堆，堆序存放在List里（Python `heapq`做法）；不承诺同优先级的先后（见D4）。

| 函数 | 签名 | 语义 | 复杂度 |
| --- | --- | --- | --- |
| `heapify` | `heapify(mut h List<T>) void` | 就地建堆，T∈int/float/string（自然序，float须有限） | O(n) |
| `push` | `push(mut h List<T>, x T) Result<int,string>` | 入堆，成功值为新长度 | O(log n) |
| `pop` | `pop(mut h List<T>) Result<T,string>` | 取最小；空为Err `HEAP_EMPTY` | O(log n) |
| `peek` | `peek(h List<T>) Result<T,string>` | 看最小（下标0）；空为Err `HEAP_EMPTY` | O(1) |
| `smallest` / `largest` | `(xs []T, k int) Result<[]T,string>` | top-k（升序/降序返回），T∈int/float/string；k>len返回全部排序结果，k<0为E4003 | O(n log k) |
| `push_by_key` | `push_by_key(mut h List<T>, x T, key fn(T) K) Result<int,string>` | 按键的最小堆（任意T，K∈int/float/string）；**同一个堆必须始终用同一键函数** | O(log n)次键回调 |
| `pop_by_key` | `pop_by_key(mut h List<T>, key fn(T) K) Result<T,string>` | 同上 | O(log n)次键回调 |

- 自然序函数只接受int/float/string元素（静态E3004、动态E4003）；按键函数是L0-1回调函数，遵循“L0-1回调合同”（回调改动同一List报E4003且不写回）。
- 堆函数不检查List是否已是合法堆（与`alg.binary_search`不扫描有序性同理）；对非堆List调用push/pop结果为某个排列但不越界、不崩溃。

## 4. 版本与实现位置

- 新函数`since` 14→HUAB14；`Deque<T>`类型名加入HUAB类型校验（出现即需14）。
- 实现：Value新变体`DequeValue`（末尾追加，不改变既有下标），按`List<T>`的接入点逐一补齐；`src/deque_stdlib.cpp`、`src/heap_stdlib.cpp`（按键版本放回调路径）。
- 不改selfhost（自举暂停，L0-4登记）。

## 5. 基准（实施前先测手写基线；`perf/stdlib/deque`、`perf/stdlib/heap`）

- deque/01 bfs_manual（网格BFS：List当队列＋头下标）→ native `push_back/pop_front`；参照Python `collections.deque`、Lua表＋头尾下标。
- deque/02 sliding_window_manual（滑动窗口最大值：两端进出）→ native。
- heap/01 heap_manual（手写`list.get/set`上滤下滤）→ native `push/pop`；参照Python `heapq`。
- heap/02 topk_manual（`alg.sorted`后取前k）→ native `smallest`；参照`heapq.nsmallest`。
- heap/03 dijkstra_by_key（结构体负载按键）→ native `push_by_key/pop_by_key`，与“编码键为int的自然序堆”对照。
- 每步`ab_bench`（core与stdlib既有，含List相关基准）无退化，可疑项交替计时复核。

## 6. 验收

1. 三路径一致：Deque两端各种交替序列与参考模型（Python `deque`）逐项一致；空pop/front/back Err；上限；只读Deque写入E3003/E4007；`clone`独立；跨Task只读快照；`==`按元素。
2. Heap：随机操作序列与Python `heapq`的取出序列一致（自然序）；`smallest/largest`与排序结果一致；按键版本与参考模型一致；空Err；非堆输入不崩溃；回调改动同一List报E4003。
3. HUAB14往返、旧程序格式不变、降级头E6002；`er0_inventory`新增ER0-Y行。
4. 基准表写入路线文档；CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变。
5. 文档：接口（签名、ER0-Y、计数）、语言设计（Deque句柄规则）、运行时（HUAB14）、路线、设计决策、概览、百科计数。

## 7. 需要你决定的点

- **D1 Deque形态**：推荐A新泛型句柄`Deque<T>`（不动List的合同；工作量约为再接一遍List）；备选B把List存储换成`std::deque`并给List加`push_front/pop_front`（不新增类型，但`list.reserve`“是否扩容”的已冻结合同要改，且List随机访问与追加可能变慢，需A/B门槛）。
- **D2 Heap形态**：推荐A作用于`List<T>`的函数（`heapq`式，不新增类型）；备选B新句柄`Heap<T>`/`PriorityQueue<T>`（可内置“入堆时算一次键”和稳定序，但又一个泛型类型）。
- **D3 按键堆的键计算**：推荐每次比较调用键函数（O(log n)次回调，堆内只存元素，与自然序版本共用List）；备选入堆时算一次键并另存（需要并行的键List或新类型，见D2-B）。
- **D4 同优先级顺序**：推荐不承诺（`heapq`同样不承诺；需要先进先出时在键里编入递增序号）；备选承诺稳定（需D2-B）。
- **D5 模块划分**：推荐`std.deque`与`std.heap`两个模块（与`std.list`同风格）；备选合并为`std.collections`（名称前缀`deque_*`/`heap_*`）。
- **D6 HUAB14**：推荐新格式号14。
- **D7 第5步顺序**：推荐collections → Duration/UTC → os exit/平台 → fs walk/copy → test/log。

## 8. 实施记录（2026-10-24）

- 全部按定稿实现（deque 11、heap 8，HUAB14）；`DequeValue`为Value变体末尾新增项。
- 与提案差异：无接口差异。deque与自然序heap在`invoke_standard`入口早分派（同random）。
- 顺带修正：`gc_cost`按种类计数数组22→24（Rng加入后已越界），加`static_assert`（仅剖析构建）。
- 观察：交替计时下部分字符串基准在新构建慢约5–7%，但非单调（更早的构建同样慢），记为代码布局波动，未归因。
- 结果表见docs/标准库设计与实现路线.md“第5步第一组Deque/Heap结果”。
