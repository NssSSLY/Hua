# HUA-D038切片2接口提案：map标准操作、list P0与Set（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，D1–D7全部按推荐（D1按键升序、D2多返回entries、D3方案B、D4复制、D5随切片2实测、D6 HUAB10、D7返回int）；先建基线基准再实施，实施完成前函数不可调用。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)“当前主线”表的map/list/Set行；Map冻结规则（语言设计与类型规则“### 1. Map”）；现有`std.list`（src/binary_stdlib.cpp）与`std.alg`（src/algorithm_stdlib.cpp）实现；旧基线`std.maps`提案（分支`bo11-capability-diagnostics-20261023`，`designs/2026-10-23-s2-native-map-primitives.md`）仅作参考。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| `MapData::entries`为`std::map<MapKey,Value>`，`MapKey=variant<bool,int64,string>` | include/hua/value.hpp:202 | 按键有序；节点逐个分配（HUA-D035热点） |
| 每次`m[k]`/`has`/`delete`都经`map_key`→`enforce_type(key)`构造一个新`MapKey`，string键会复制一次 | src/value.cpp:593–600 | “查找不复制键”需改内部比较器，见§1.1 |
| Map上限100万条，超出E4099 | src/value.cpp:609/614 | 新函数沿用 |
| List为`std::vector<Value>`，越界与上限走`Err`（`list index out of bounds`/`list item limit exceeded`），`get/snapshot`对元素`copy_value(read_only)` | src/binary_stdlib.cpp | 新list函数保持同一错误风格 |
| `mutates_first`的运行时写能力检查目前只认Buffer/List | src/stdlib.cpp invoke_standard | 需扩到MapValue（只读Map报E4007，与`delete`一致；静态E3003） |
| 泛型签名只对alg/simd/task/list做按参数类型特化 | src/binary_stdlib.cpp `standard_signature` | 新增maps/sets分支即L0-2的落地，sema经同一函数检查 |
| `std.alg`比较只支持int/float/string（float须有限），稳定归并、工作上限3200万、每1024次取消检查 | src/algorithm_stdlib.cpp | `list.sort`复用同一比较与上限 |
| 语言有多返回`(T1,T2)`但无Tuple，有显式用户泛型但标准库无泛型struct | 语言设计“多返回”“用户泛型” | 影响`entries`形态，见决定点D2 |

## 1. map标准操作：新建`std.maps`

模块名与Go `maps`对应；map本身仍是内建类型，这里只加函数。K∈{bool,int,string}，V为Map允许的任意值类型。签名中的`map[K]V`由`standard_signature`按第一个参数的实际类型特化，第二个及以后的参数必须严格同型（不做隐式转换，直接整数字面量到float沿用Map写入规则）。

| 函数 | 签名 | 语义 | 复杂度/分配 |
| --- | --- | --- | --- |
| `maps.keys` | `keys(m map[K]V) []K` | 返回只读slice，**按键升序**（见D1） | O(n)；一次分配n项；string键各复制一次 |
| `maps.values` | `values(m map[K]V) []V` | 只读slice，顺序与`keys`逐项对应；元素按读取规则（嵌套容器只读共享，不深复制） | O(n)；一次分配 |
| `maps.entries` | 推荐`entries(m map[K]V) ([]K,[]V)` | 一次调用同时得到对齐的键与值两份只读slice（见D2） | O(n)；两次分配 |
| `maps.get_or` | `get_or(m map[K]V, key K, default V) V` | 存在返回其值（V可空时存在的nil返回nil），缺失返回default；不插入 | O(log n)；不复制键（§1.1） |
| `maps.pop` | `pop(mut m map[K]V, key K) V?` | 删除并返回旧值；缺失返回nil且不改Map；V已可空时不重复加`?`，需区分时先`has` | O(log n)；需写能力 |
| `maps.merge` | `merge(mut dst map[K]V, src map[K]V) int` | 把src每条写入dst（同键src覆盖），返回**新增**键数；`dst`与`src`为同一存储时为无操作返回0；新增后超过100万条E4099且dst不变（先计数再写） | O(m·log(n+m))，src键均大于dst最大键时走尾部hint为O(m) |

- 纯/可变成对原则：`pop`/`merge`是可变版本；纯版本用现有写法`let c=clone(a)`后再`merge`，不另加`merged`（避免接口面翻倍，若要可在P1补）。
- 结果能力：`keys/values/entries`返回只读slice（与`list.snapshot`一致）；它们是快照，之后修改Map不影响已返回的slice。
- 错误：全部无业务Err（不返回Result）。类型不符静态E3004/动态E4003；只读Map上`pop/merge`静态E3003、动态E4007；超上限E4099。
- 工作上界：单次调用元素操作≤100万（Map上限本身即上界），每1024次做Task取消检查点。

### 1.1 内部优化：查找不复制键（语义不变）

- 改`std::map<MapKey,Value>`为`std::map<MapKey,Value,MapKeyLess>`，`MapKeyLess`为透明比较器（`is_transparent`），可直接用`bool/int64/std::string_view`的轻量视图查找；`map_key`在只读路径（`m[k]`、`has`、`get_or`、`pop`/`delete`的查找）改为返回视图，只有真正插入时才构造拥有型`MapKey`。
- 顺序与比较结果与现在逐位相同（同一全序：先按variant下标，再按值；string按字节），因此遍历、`==`、JSON、HUAB全部不变。
- 先用基准测出收益再定是否保留（性能日志规则）；收益不显著则回退，只保留新函数。

## 2. list P0（`std.list`增量）

全部按`List<T>`特化；可变函数要求`mut`/可写List（静态E3003、动态E4007），越界与上限沿用现有`Err`文本风格。

| 函数 | 签名 | 语义 | 复杂度 |
| --- | --- | --- | --- |
| `list.insert` | `insert(mut l List<T>, i int, v T) Result<int,string>` | 0≤i≤len，插到i之前，返回新长度；越界Err `list index out of bounds`，满100万Err `list item limit exceeded` | O(len−i) |
| `list.remove_at` | `remove_at(mut l List<T>, i int) Result<T,string>` | 删除并返回第i项；越界Err | O(len−i) |
| `list.index_of` | `index_of(l List<T>, v T) int` | 第一个`==`相等项下标，未找到−1；相等按语言`==`（结构相等，float按IEEE，NaN不等） | O(n) |
| `list.contains` | `contains(l List<T>, v T) bool` | 同上的存在判断 | O(n) |
| `list.reverse` | `reverse(mut l List<T>) void` | 就地反转 | O(n)，不分配 |
| `list.sort` | `sort(mut l List<T>) void` | 就地稳定升序；T∈{int,float,string}，非有限float或其他T为E4003；比较工作超3200万E4099且**List不变**（先在副本上排好再交换存储） | O(n log n)；一次n项临时缓冲 |
| `list.swap` | `swap(mut l List<T>, i int, j int) Result<bool,string>` | 交换两项，i==j返回ok(true)；越界Err | O(1) |
| `list.truncate` | `truncate(mut l List<T>, n int) void` | 保留前n项；n≥len无操作；n<0为E4003 | O(len−n) |
| `list.slice` | `slice(l List<T>, start int, end int) Result<[]T,string>` | 0≤start≤end≤len，返回该区间的只读slice**副本**（见D4）；越界Err | O(end−start) |

- `sort`只做默认升序；`sort_by`/比较器依赖L0-1回调，留P1。`reverse`与`std.alg.reverse`区别：后者对slice返回新副本，前者就地。
- `index_of/contains`不复用`std.alg`（后者限int/float/string且限slice），用通用`==`，覆盖struct/嵌套元素。

## 3. Set（决定点D3）

| 方案 | 做法 | 成本 | 性能 |
| --- | --- | --- | --- |
| **推荐B：`std.sets`函数，Set即`map[K]bool`的键集合** | 增删查用现有`m[k]=true`/`delete`/`has`/`len`/`for k in m`；新增集合运算函数（下表）；值被忽略，结果的值统一为true | 只加函数，无新类型；不碰GC、clone、`==`、JSON、HUAB类型名、sema类型系统 | 与原生Set同一棵有序树；集合运算为线性有序归并 |
| A：新运行时类型`Set<K>`（类似List） | `sets.new/from_slice/add/remove/has/len/snapshot`＋运算 | 新Value变体、GC追踪、类型串`Set<K>`、复制/相等/打印/JSON/HUAB/sema全链路，工作量约为B的3倍 | 与B相同（底层都是有序树），只多一层类型区分 |

方案B的函数（K∈{bool,int,string}，两参数须同K；值类型V任意，只看键）：

| 函数 | 签名 | 语义 | 复杂度 |
| --- | --- | --- | --- |
| `sets.from_slice` | `from_slice(xs []K) map[K]bool` | 去重建集合（可写新Map） | O(n log n)；已排序输入O(n) |
| `sets.union` | `union(a map[K]V, b map[K]V) map[K]bool` | 键并集 | O(n+m)，有序归并＋尾部hint插入 |
| `sets.intersection` | `intersection(a, b) map[K]bool` | 键交集 | O(n+m) |
| `sets.difference` | `difference(a, b) map[K]bool` | 在a不在b | O(n+m) |
| `sets.is_subset` | `is_subset(a, b) bool` | a的键全在b中 | O(n+m)，提前退出 |
| `sets.is_disjoint` | `is_disjoint(a, b) bool` | 无公共键（Python `isdisjoint`；也覆盖旧提案`intersects`） | O(n+m)，提前退出 |

- 运算结果均为新的可写Map，超100万条E4099。可变版本（`union_into`等）留P1，按实测需要再加。
- 若选A，B的函数名与语义可原样迁移到`Set<K>`，不浪费。

## 4. 性能原则落实与基准（实施前先建基线）

- 一次原生调用完成一件事；单次工作有界（≤100万元素/3200万比较）；不做隐藏复制：`keys/values/slice/entries`的复制是函数的明确合同，`get_or/pop/has`不复制键。
- 新基准（L0-3框架，`--suite stdlib`，每项含`.hua/.py/.lua/.expect`，Python用dict/list/set，Lua用table）：
  - maps/01 lookup_string_keys（`m[k]`与`has`热循环，衡量§1.1）、maps/02 keys_values_manual→native、maps/03 get_or_pop、maps/04 merge；
  - list/01 insert_remove、list/02 index_contains、list/03 sort_reverse_inplace（对比`alg.sorted`＋重建）；
  - sets/01 union_intersection_manual→native、sets/02 dedup（`from_slice`对比手写循环）。
- 先用现有接口写“手写版”测基线（与切片1的01–08同法），实施后同输入A/B；core套件做A/B确认无回退。

## 5. 版本与三路径

- 新函数`since=10`→**HUAB10**；读取器接受1–10，引用这些函数的产物写10，旧程序格式不变；需求10的报错文本同HUAB9写法。不复用9，避免“格式9”在切片1与切片2之间含义不同。
- AST解释器、源码VM、HUAB三路径经同一`invoke_standard`，测试仍逐路径对照（沿用`tests/strings_p0_tests.py`的写法，新建`tests/collections_p0_tests.py`）。
- 不改selfhost（自举暂停；L0-4登记同步`signatures.hua`/`archive.hua`）。

## 6. 验收

1. 三路径输出一致：空/单项/重复/边界下标/只读输入/V可空/`dst is src`/超上限不变更/取消。
2. 静态：类型不符E3004，只读写入E3003；`er0_inventory`增ER0-U行（与ER0-T同法）。
3. HUAB10往返；旧程序头仍<10；降级头E6002拒绝。
4. §1.1：Map遍历、`==`、JSON、HUAB输出逐字节不变；lookup基准有可测收益才保留。
5. 基准表与参考语言倍数写入路线文档；CTest全套（已知archives_extensions除外，HUA-D037）。
6. 文档：标准库与内建接口（签名、错误、ER0增量、计数）、语言设计（只在D1承诺顺序时补一行）、运行时（HUAB10）、路线、设计决策、概览。

## 7. 需要你决定的点

- **D1 keys/values的顺序**：推荐“按键升序”作为这三个函数的合同（与现状一致、输出可复现；若HUA-D004将来把Map换成哈希，这三个函数改为O(n log n)排序，成本写进文档）；`for k in m`仍不承诺顺序。备选：只承诺`keys`与`values`彼此对齐、不承诺升序。
- **D2 entries形态**：推荐多返回`([]K,[]V)`（不引入Tuple，一次调用；实施时需让标准函数支持多返回结果，若sema/HUAB改动超出预期则退为备选）；备选：不提供`entries`，文档说明`keys`/`values`对齐。
- **D3 Set**：推荐B（`std.sets`函数操作`map[K]bool`），备选A（新`Set<K>`类型）。
- **D4 list.slice**：推荐复制区间的只读slice（O(k)，安全）；真正零复制视图需List存储共享与重分配失效规则，另立后置。
- **D5 §1.1查找不复制键**：随切片2实施并以基准决定去留（推荐），或单独另做。
- **D6 HUAB10**：推荐新格式号10（备选：并入9，要求切片1与2同时视作同一版本）。
- **D7 merge返回值**：推荐返回新增键数`int`（备选`void`）。

## 8. 实施记录（2026-10-24）

- 实现：`src/collections_stdlib.cpp`（maps/sets与list新增9个）；`standard_signature`特化maps/sets/list新签名；sema增map/集合键/排序元素检查；`invoke_standard`对maps/sets直接分派（不构造签名字符串）；HUAB10读写。
- D2：多返回entries无需改sema/HUAB，标准函数返回`multi<[]K,[]V>`即可经既有`let a,b=`绑定。
- **D5结果：已回退**。透明比较器（`MapKeyView`＋`is_transparent`）实测：长键（>SSO）`m[k]`/`has`热循环+8%，但短键（"key123"这类SSO键，原复制本不分配）-7%，且与map无关的core 07_strings -4%、05_map_ops -6%（同源只差D5的三方交替计时：base 0.2226/无D5 0.2256/D5 0.2400秒；07：0.3388/0.3360/0.3509）。按“收益不显著则回退”保留无D5版本，只留`map_find`作为读取单一入口（行为与原`find(map_key)`相同）。
- 与提案差异：HUAB降级检查由逐条标准引用先报“unsupported standard library reference”（E6002），整体“require HUAB10”文本在其后，不会先出现。
