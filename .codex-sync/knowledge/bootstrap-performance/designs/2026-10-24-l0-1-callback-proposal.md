# HUA-D038第3步接口提案：L0-1回调入口与首批回调方法（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，E1–E8全部按推荐（E1两种排序都给、E2 alg/list/maps分放、E3普通值且min/max_by_key用Result、E4 find与find_index、E5修订计数检测、E6全注解、E7原型门槛、E8 HUAB11）；先做`alg.map`/`fold`原型测量，达标后再实施其余。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)“前置基础”L0-1行与对齐清单中依赖L0-1的项（list/alg的`sort_by`/`map`/`filter`/`fold`/`find`/`any`/`all`/`min_by`/`max_by`，maps的`update`）；现有实现`src/vm.cpp`、`src/interpreter.cpp`、`src/task_engines.cpp`、`src/stdlib.cpp`、`src/sema.cpp`、`src/gc.cpp`。格式沿用[切片2提案](2026-10-24-slice2-map-list-set-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 标准函数全部经同步的`invoke_standard`执行，拿不到执行器，无法调用Hua函数 | src/stdlib.cpp | 回调类函数不能放进现有`invoke_standard`，需要执行器提供的入口 |
| VM已有同线程重入调用：`co_await call(f,args,site); co_await dispatch(depth)`后弹出结果（task包装用） | src/task_engines.cpp `VirtualMachine::launch` | 回调入口可直接复用，帧、步数、调用深度、异常传播都走既有路径 |
| 解释器有`Routine<Value> call(const Callable&,args,const Node& site)`；`task_start`已示范用合成`Node(Call,span)`作为调用点 | src/interpreter.cpp | 解释器侧同样可复用 |
| VM的`Op::Call`对`builtin_may_suspend`为真（或`task_start`/`parallel_map`）的内建走协程`call()`，其余同步调用 | src/vm.cpp Op::Call | 回调类标准函数标记为“需协程”即可走协程路径，不新增opcode，HUAB只需`since` |
| 函数值的静态类型是名义的`fn:<名字>`，sema可由`model_.functions`取到参数与结果注解；局部函数/闭包同样是`fn:` | src/sema.cpp:183/364/536 | 回调签名可静态检查（数量E3005、类型E3004），无需引入函数类型语法 |
| 闭包捕获局部只读快照，调用时非`mut`参数按只读传入 | 语言设计“局部函数和结构” | 回调不能修改传给它的元素；只有经模块全局才可能改到原容器（见D5） |
| GC为试删除（trial deletion）：被原生`shared_ptr`/`Value`持有的对象`use_count`高于堆内引用数即为根 | src/gc.cpp:141 | 回调期间原生局部持有的值自动存活，**无需显式登记GC根** |
| 每次`dispatch(depth)`与`call()`各是一个独立分配的协程帧（Routine无池化分配器） | include/hua/routine.hpp | 每次回调约1–2次堆分配；普通Hua循环里的函数调用走`enter_function`不分配协程帧。**这是L0-1最大的性能风险**，见§4 |
| 解释器`tick`与VM都按执行的节点/指令计步，并在安全点检查取消 | src/interpreter.cpp:30 | 回调体的步数自然计入同一执行器预算；标准调用本身仍计常数步 |

## 1. L0-1：统一回调入口（内部接口，不对用户公开）

```cpp
// include/hua/callback.hpp（新）
struct CallbackShape { std::vector<std::string> parameters; std::string result; };
struct CallbackHost {
    // 以普通调用语义执行一次Hua函数值：参数按声明类型检查/复制/只读化，
    // 步数、调用深度、取消、诊断上下文都走执行器既有路径。
    virtual Routine<Value> invoke(const Callable& f, std::vector<Value> args, const SourceSpan& site) = 0;
    // 由声明取参数与结果注解；未注解参数/结果、mut参数、async函数在此拒绝（E4003）。
    virtual CallbackShape shape(const Callable& f, const SourceSpan& site) = 0;
};
Routine<Value> invoke_callback_standard(const StandardFunction&, std::vector<Value> args,
                                        const SourceSpan&, CallbackHost&, RuntimeContext*);
```

- **VM实现**：`depth=frames_.size(); base=stack_.size(); enter_function(f,args,site); co_await dispatch(depth);` 后取结果（直接`enter_function`而非经`call()`协程，每次回调少一次协程分配）。
- **解释器实现**：用合成`Node(Call,span)`调`call()`。
- **路由**：`StandardFunction`增加`callback`标记（`since` 11）；`builtin_info`对其置“需协程”，VM `call()`与解释器`builtin()`遇到时调用`invoke_callback_standard(...,*this)`。算法只写一份C++协程，三路径（AST解释器、源码VM、HUAB）天然一致。
- **预算与取消**：回调体指令照常计步并受同一步数上限；回调帧计入128层调用深度（回调里再调回调类函数可嵌套，受同一深度上限）；原生循环每1024次回调或比较调用`task_checkpoint`。
- **诊断传播**：回调抛出的诊断原样向外传播，并追加上下文“called from std.<模块>.<函数> callback”；原生函数在传播前保证自身状态一致（见各函数“失败时”）。
- **回调约束（D6）**：回调须是参数与结果都有类型注解的普通Hua函数（具名、局部或闭包），参数个数与种类按各函数表；不允许`mut`参数、async函数或返回多值。静态E3005/E3004，动态E4003。
- **Result**：回调按其声明结果返回普通值；回调内部的`?`只作用于回调函数自身（返回类型须为Result才可用），不会穿透到标准函数。可返回Result的“try_”变体留P1。

## 2. 首批回调方法（`since` 11）

T/U/A为说明记号；签名按第一个参数的实际类型与回调声明特化（L0-2的`standard_signature`扩展）。元素以只读方式传给回调。

### 2.1 std.alg（纯函数，作用于`[]T`，返回新的只读Slice；List用`list.snapshot`取得输入）

| 函数 | 签名 | 语义 | 回调次数 |
| --- | --- | --- | --- |
| `map` | `map(xs []T, f fn(T) U) []U` | 逐项变换，U取自f的声明结果 | n |
| `filter` | `filter(xs []T, keep fn(T) bool) []T` | 保留keep为true的项，保持顺序 | n |
| `fold` | `fold(xs []T, init A, f fn(A, T) A) A` | 从左到右归约；空输入返回init | n |
| `any` | `any(xs []T, pred fn(T) bool) bool` | 有一项为true即停 | ≤n |
| `all` | `all(xs []T, pred fn(T) bool) bool` | 有一项为false即停；空为true | ≤n |
| `find_index` | `find_index(xs []T, pred fn(T) bool) int` | 第一个满足项下标，无则−1 | ≤n |
| `find` | `find(xs []T, pred fn(T) bool) T?` | 第一个满足项，无则nil（T已可空时不重复加`?`，需区分用`find_index`） | ≤n |
| `sorted_by_key` | `sorted_by_key(xs []T, key fn(T) K) []T` | 按key稳定升序，K∈{int,float,string}，float须有限 | **n**（先算键再排序） |
| `sorted_by` | `sorted_by(xs []T, less fn(T, T) bool) []T` | 按less稳定排序（自底向上归并，less须为严格弱序，不满足时结果为某个排列但不报错） | O(n log n) |
| `min_by_key` / `max_by_key` | `min_by_key(xs []T, key fn(T) K) Result<T,string>` | 键最小/最大的第一项；空输入Err `ALG_EMPTY`（与`alg.min/max`一致） | n |

### 2.2 std.list（就地，要求`mut List<T>`）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `sort_by_key` | `sort_by_key(mut l List<T>, key fn(T) K) void` | 就地稳定排序，n次回调 |
| `sort_by` | `sort_by(mut l List<T>, less fn(T, T) bool) void` | 就地稳定排序 |
| `retain` | `retain(mut l List<T>, keep fn(T) bool) int` | 就地保留满足项，返回删除个数 |

失败时（回调诊断、取消、工作超限）List不变：先在入口快照上算出排列/保留集合，最后一次写回。

### 2.3 std.maps

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `update` | `update(mut m map[K]V, key K, default V, f fn(V) V) V` | `m[key]=f(存在?旧值:default)`并返回新值；计数器写法`maps.update(counts, w, 0, inc)`。先读、再回调、后写入 |

### 2.4 共同规则

- 工作上界：单次调用回调与比较总数≤3200万（E4099），元素≤100万（沿用Slice/List上限）。
- 类型：回调结果与声明不符E4003；`sorted_by_key`的K不在int/float/string静态E3004/动态E4003。
- 复杂度如表；输出一次分配（`map`/`filter`预留n项）。
- 能力分析：sema把`alg.map(xs, f)`等视为对f的调用来合并f的写效果摘要（f可能写模块全局）；具体接入点在`sema.cpp`标准调用分支与`pending_effects_`，实施时以C04/A07既有用例验证不漏报。

## 3. 版本与产物

- 新函数`since`=11→HUAB11，读取1–11；不新增opcode、不改字节码布局；引用旧函数的程序格式不变。
- 不改selfhost（自举暂停；L0-4登记）。

## 4. 性能原则、风险与基准

- **风险**：每次回调是一次VM重入（`enter_function`＋一个`dispatch`协程帧）。若协程帧分配使回调比“手写for循环里直接调同一函数”还慢，那么`map/filter/fold`只省了循环指令、却多了分配，收益可能为负。
- **实施第0步先做原型测量**（只实现`alg.map`与`alg.fold`）：同输入对照手写`for x in xs { list.append(out, f(x)) }`与`for`累加。门槛：原生版不慢于手写版；若慢，先做“回调帧复用”（例如为Routine增加按大小的空闲帧池，或让一次标准调用内的多次回调复用同一个dispatch协程），复测达标后才继续其余函数；仍不达标则回报用户再定。
- `sorted_by_key`优先于`sorted_by`：前者n次回调，后者n log n次；文档推荐用key。
- 基准（`perf/stdlib/alg`、`perf/stdlib/list`，每项`.hua/.py/.lua/.expect`，实施前先测手写基线）：
  - alg/01 map_filter_manual→native（Python `[f(x) for x in xs if p(x)]`，Lua for）
  - alg/02 fold_manual→native（Python `functools.reduce`与for）
  - alg/03 any_all_find（短路命中在前/中/后）
  - alg/04 sort_by_key_vs_by（结构体按字段排序：手写为键数组+`alg.sorted`+回填；对照`sorted(key=)`）
  - list/07 retain_manual→native；maps/08 update_counter（单词计数，对照`dict.get(w,0)+1`/Counter）
- 每步A/B：core与stdlib既有基准无退化（可疑项用交替计时复核，教训见切片2）。

## 5. 验收

1. 三路径一致：空输入、单项、短路位置、回调抛诊断（传播与上下文、List不变）、回调取消、嵌套回调（回调内再调`alg.map`）、调用深度上限、步数上限、闭包回调与局部函数回调、只读元素不可写。
2. 静态：回调参数个数E3005、类型E3004、未注解/`mut`参数/async回调E3004；`mut`要求E3003。
3. HUAB11往返、旧程序格式不变、降级头E6002。
4. 能力分析：回调写模块全局时效果不漏（C04相关用例）。
5. 基准表与参考语言倍数写入路线文档；CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变。
6. 文档：接口（签名、ER0-V增量、计数）、语言设计（回调调用规则）、运行时（回调入口、HUAB11）、路线、设计决策、概览。

## 6. 需要你决定的点

- **E1 排序回调形态**：推荐两者都提供、文档推荐`*_by_key`（n次回调，性能优先）；备选只提供`sorted_by_key`/`sort_by_key`，比较器版后置。
- **E2 放置**：推荐纯函数放`std.alg`（作用于Slice，List经`snapshot`），就地版本放`std.list`，`update`放`std.maps`；备选在`std.list`上再各加一份`map/filter`（接口面翻倍）。
- **E3 返回形态**：推荐无业务错误的函数返回普通值（`map`/`filter`/`sorted_by*`不包Result），`min_by_key/max_by_key`空输入返回`Result`的`ALG_EMPTY`以与`alg.min/max`一致；备选全部不包Result、空输入返回`T?`。
- **E4 find**：推荐同时提供`find`（`T?`）与`find_index`（int）；备选只提供`find_index`。
- **E5 就地函数中回调经全局改动同一List**：推荐失败安全语义——在入口快照上计算、最后一次写回，若期间List被改动（加一个修订计数检测）报E4003“list modified during callback”且不写回；备选不检测、以写回覆盖回调期间的改动（Python排序期间清空列表的做法更接近前者）。
- **E6 回调约束**：推荐要求参数与结果全注解、禁`mut`参数/async/多返回；备选允许未注解参数（运行时才检查）。
- **E7 性能门槛**：推荐原型阶段“原生不慢于手写循环”为继续条件，不达标先做回调帧复用；备选先全部实现再优化。
- **E8 HUAB11**：推荐新格式号11（与切片1/2一致的做法）。

## 7. 原型测量（2026-10-24，E7门槛）

- 实现（未提交）：`include/hua/callback.hpp`、`src/callback_stdlib.cpp`（`alg.map`、`alg.fold`，`since` 11）；`StandardFunction::callback`标记，`builtin_info`对其置“需协程”（tasks.cpp镜像同步）；VM `Callbacks`宿主（`enter_function`＋`dispatch(depth)`，清空调用点contextual标记）、解释器宿主（合成`Node(Call,span)`调`call()`）；sema按回调声明检查参数个数E3005、注解/`mut`/结果E3004、元素与累加器类型E3004，结果类型取自回调声明；能力分析暂按未知调用保守处理（`pending_effects_.unknown`＋`forget_unknown`），完整实施时再细化；archive读取HUAB11。
- 三路径：AST解释器、源码VM、删除源码后的HUAB11输出一致；含闭包回调、嵌套`map(map())`、空输入。错误：回调内除零E4004原样传播（trace已含标准调用点“called from”，故不另加上下文）；回调递归经`fold`超过128层报E4099。
- 计时（同一构建交替各9次中位数，含进程启动与两万项List构建）：

| 对照 | 手写 | 原生 | 加速 | VM步数 |
| --- | ---: | ---: | ---: | --- |
| 01手写for+`list.append` → 03 `alg.map` | 0.480秒 | 0.171秒 | 2.8x | 744万→264万 |
| 02手写for累加 → 04 `alg.fold` | 0.175秒 | 0.160秒 | 1.09x | 664万→264万 |

- 结论：**E7门槛通过**——每次回调重入（一个`dispatch`协程帧）不比Hua循环里的普通调用慢；`map`的收益主要来自省掉逐项`list.append`标准调用。暂不做回调帧复用优化（收益上限约为fold的剩余循环开销，留待其余函数实施后按剖析决定）。
- core `ab_bench`对切片2构建0.99–1.05x（07_strings 0.93x为已知的ab_bench顺序波动，切片2已用交替计时排除）。strings/collections测试仍全过；`er0_inventory`因两个原型函数尚无ER0-V行而失败，完整实施时补。
- 下一步：按定稿实施其余13个函数（alg的filter/any/all/find/find_index/sorted_by_key/sorted_by/min_by_key/max_by_key，list的sort_by_key/sort_by/retain，maps.update）、ListData修订计数（E5）、三路径测试、ER0-V行、基准（alg/05起、list/07、maps/08）与文档。

## 8. 完整实施（2026-10-24）

- 15个函数全部实现（alg 11、list 3、maps 1），`ListData::revision`实现E5（std.list写函数在`invoke_standard`统一递增，`reserve`除外；就地回调函数每次回调后检查）。
- 性能：结果表见docs/标准库设计与实现路线.md“L0-1回调结果”。`sorted_by`（比较器）与`maps.update`慢于最快的手写写法，原因与数字已写明；文档推荐`*_by_key`。
- 尝试并回退：Routine每线程协程帧池（`update`无变化）。保留：`Op::Call`直接执行回调函数、宿主只带span、`callback_shape`不分配参数串、回调复用调用点FunctionRoute。
- 缺陷与修正：回调帧`site_ref`曾指向宿主内span，错误展开时悬空（深递归崩溃）；改指字节码span或复制。
- 能力分析仍按未知调用保守处理（`pending_effects_.unknown`＋`forget_unknown`），未细化为“合并回调写效果摘要”；精度不足时可能保守地失去事实，不会漏报。
