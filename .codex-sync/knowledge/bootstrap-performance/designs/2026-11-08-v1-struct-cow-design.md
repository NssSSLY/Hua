# 2026-11-08 V1 结构体写时复制（COW）细化设计

- 状态：**设计稿，待用户审定**；审定前不改代码。
- 上级：[底层复核提案](2026-11-08-vm-lower-layer-review-proposal.md)。用户2026-11-08选择“Yes, design V1 first”，即接受新次序（D1）并要求先出细化设计（D2）。
- 范围：普通struct与enum实例（`StructValue`/`StructData`）。固定数组、Result载荷、Map/List元素存储方式不在V1内；`PlainRecord`保持现状。
- 合同：字节码、HUAB、步数、程序输出、错误码与位置、C2产物逐字节不变。AST解释器保持现行急切复制，作为三路径中的独立对照。

## 1. 设计输入（实测）

计数版`tests/build-count`（`-DHUA_INTERNAL_PROFILE=ON`，Release，当前工作区含S2-2a），运行`hua_performance_probe perf/results/compiler.huab selfhost/ast_pool.hua`，共35,389,144步：

| 计数 | 值 | 含义 |
|---|---|---|
| `copy_struct` | 1,737,545 | struct复制次数，每次新分配一个结构盒 |
| `copy_struct_fields` | 8,603,694 | 被复制的字段数 |
| `ops.LocateField` | 173,532 | 字段写入次数 |
| `managed_allocations` | 2,791,807 | 托管分配次数，共296 MB（`tracked_object_bytes`） |
| `gc_nanoseconds` | 230 ms | 运行2.56秒中的GC时间，约9% |
| `load_kinds.struct` | 3,421,062 | 把struct压栈的Load次数 |
| `ops.Member` | 3,615,291 | 字段读取次数 |
| `type_argument_parses` | 2,557,833 | 运行时解析类型串次数（V2的对象） |

**struct复制约是字段写入的10倍。** 编译器代码大量把结构“传进去读”，只有少数会改。COW让读的路径不再复制，只有真正写到被共享的结构时才复制一次。

## 2. 现状机制（为什么不能只改copy_value）

当前实现是“**栈上别名＋边界复制**”：

- Load、Member、Index把结构句柄（`shared_ptr<StructData>`）原样压栈，栈上的值与变量、字段或容器元素**是同一个结构盒**。
- 字段赋值`a.b.c = v`编译为`Load a; Member b; LocateField c; …; Store`（`src/bytecode.cpp`的`location()`，IR路径相同）。LocateField拿到的是Member压出的**别名**，直接改这个盒子，修改才会落到`a`里。同理`m[k].x = v`、`s[i].x = v`依赖Index返回的别名。
- `mut`参数（`enforce_borrow`）和可变方法的`self`也是别名。
- 值语义靠“在边界复制”实现：Bind、Store、实参、返回、unwrap、标准库取出值，都调用`copy_value`，每次新建结构盒。字段层已有部分共享（`RecordFields`的共享Flat、`exposed`标记、`copy_depth`、`PlainRecord`），但外层结构盒每次都新分配，且`exposed`一旦置位就永久改为急切复制。

所以如果只把`copy_value`改成共享句柄，`var y = x; y.f = 1`会经别名直接改到`x`。COW必须同时回答“写入时怎么确认结构没被别人共享”。

### 2.1 与2026-10-29“值复制第一优先”的区别

性能文档9.2节D早已规划struct COW。2026-10-29实施了其中一部分（[运行记录](../runs/2026-10-29-value-snapshots.md)），真实编译没有提速（ast_pool 0.99x）。原因与V1的对策如下：

| 2026-10-29的做法与结果 | V1的对策 |
|---|---|
| 共享的是**字段体**，外层结构盒仍每次新分配；首版全字段共享使ast_pool的GC扫描对象984,636→1,608,547，完整回收15→28次，因为共享字段体成了额外的图节点 | 共享**整个结构盒**（句柄加一），不新建任何包装对象。图节点数只会减少；验收新增闸门：`gc_records`与扫描对象数不得增加 |
| 已发布字段引用（`exposed`）永久转为急切复制 | 钉住只在写路径存活期间有效（3.3） |
| readonly能力投影保守复制 | 用句柄上的`fields_readonly`位延迟投影（3.4） |
| 嵌套记录/Result/固定数组外层保守复制 | 嵌套结构随外层整体共享；Result与固定数组不在V1范围 |

## 3. 新模型

**句柄即值，写入走写路径。**

- **共享复制**：非深复制的`copy_value(struct)`只复制句柄（引用计数加一），不分配。被共享的结构盒在逻辑上是不可变的。
- **写准备**：任何经别名写入结构盒之前，沿“写路径”（变量槽→字段槽→…，或容器元素槽→…）逐层确认独占。独占就原地写；被共享就浅复制一层，把新盒放回所在的槽，再继续向下。这就是Swift的做法。
- **钉住（pin）**：写准备完成到写入结束之间，被写的结构盒处于“钉住”状态（`StructData::pins>0`）。钉住的盒子不能被共享复制：`copy_value`对它走现行急切复制路径。这样可以保证写入前、右侧求值期间新产生的副本不会看到后来的写入。这正是现行别名语义的效果，只是从“永久暴露”变为“写入期间有效”。

### 3.1 不变量

| 编号 | 不变量 |
|---|---|
| I1 | 未钉住的结构盒若被两个以上强句柄持有，任何写入都不得原地进行 |
| I2 | 钉住期间，盒子的所有现存强句柄要么是写路径上的有意别名（Location、借用、接收者、方法值），要么是钉住之前已存在、按现行语义本来就会看到别名写入的值；钉住期间新做的复制一律急切复制 |
| I3 | 只读句柄（`writable=false`）永远不能成为写路径的起点 |
| I4 | 共享复制不改变能力：只读来源复制出的可写副本，其共享成员仍按A07-1保持只读（见3.4） |
| I5 | 任一结构盒的可见内容只因经过写路径的写入而改变 |
| I6 | 引用计数用于判定独占，但只把强引用计入：GC记录与`FieldAccess`缓存只持有弱引用，计数器不计入 |

### 3.2 写准备的判定

对一个写路径上的槽`slot`（持有句柄`h`，指向盒子`B`）：

1. `B->pins>0`：已在写路径上（借用、接收者、外层写入），原地写，不复制。
2. 否则`h.data.use_count()==1`：只有这个槽持有，原地写，并钉住。
3. 否则：浅复制`B`为`B'`，即新建一个盒子并共享复制各字段，嵌套结构字段因此各自引用计数加一；把`slot`改为指向`B'`，钉住`B'`。

计数偏高（例如栈上残留的临时句柄）只会导致多复制一次，不会出错；判定是保守的。

### 3.3 钉住的寿命

钉住标记属于写路径上的句柄本身（`StructValue`新增`pinned`位）。C++拷贝构造不继承钉住，析构或移出时`pins`减一。寿命如下：

| 写路径 | 钉住由谁持有 | 何时释放 |
|---|---|---|
| 字段/下标赋值（含复合赋值、多重赋值） | 栈上的写准备句柄，再转交给`Location` | Store/StoreMulti消费Location；异常与Unwind丢弃栈项时由析构释放 |
| `mut`实参 | 被调用者绑定中的借用句柄 | 被调用者返回后其环境清空（S2-2a的`recycle_scope`或删除器） |
| 可变方法接收者 | `Callable::receiver`及`self`绑定 | 调用结束；若方法值被保存（`let m = obj.mutate`），随方法值存活，与现行“永久别名”效果一致 |
| 多层写路径 | 每层各自钉住 | 随所在Location一起释放 |

### 3.4 只读能力与COW

现行规则（A07-1）是：只读来源的复制可以改自身的值字段，但共享成员（Slice、Map、List、Buffer等）仍只读。现在的实现是在复制时对每个字段做`read_only`。

COW下不再复制字段，改在句柄上记录：`StructValue`新增`fields_readonly`位。

- 从只读来源共享复制：新句柄`writable=true`、`fields_readonly=true`。
- 经`fields_readonly`句柄读字段：对引用型字段应用`read_only`；嵌套结构字段返回的句柄同样带`fields_readonly`。
- 写准备时若句柄带`fields_readonly`：先浅复制（即使独占也复制一次，避免改到他人仍以可写视角持有的共享成员），复制时对引用型字段应用`read_only`，然后清除该位。
- 比较、`show`、哈希、JSON只读不写，不受影响。

### 3.5 加载时分类：哪些读值不需要写准备

写准备有成本（检查、钉住、可能复制），只在可能写入时做。VM在`code_layout`中对每段Code做一次前向栈模拟，为每个栈槽记录生产它的指令，再按消费者给生产者分类：

| 消费者 | 生产者（Load/Member/Index产出的结构句柄）分类 |
|---|---|
| LocateField、LocateIndex的基值 | 写 |
| 结果被分类为“写”的Member/Index的基值 | 写（逐层向上传播） |
| 作为Call被调者的Member的基值（可能是可变方法） | 写 |
| Call实参：静态可知被调者（Load函数名、标准库`mutates_first`）且该参数不是`mut` | 读 |
| Call实参：参数为`mut`，或被调者不可静态确定 | 写 |
| Bind、BindMulti、Store的值、Return、Binary、Unary、条件跳转、MakeArray、ArrayAppend、MakeMulti、MapInsert、InitField、Defer实参、Propagate、IterNext/SliceInit的来源、Pop | 读 |
| 控制流汇合导致来源不唯一，或模拟遇到无法识别的情形 | **写（默认）** |

**安全性来自默认值**：分类为“写”等价于现行别名语义（写准备＋钉住），所以分类失误只会少些优化，不会出错。分类为“读”的生产者只压普通句柄；它的消费者都会经`copy_value`得到共享副本，或只读不写。分类结果存为`CodeLayout`的逐指令位，HUAB不变。

### 3.6 其他边界

- **GC**：结构盒仍是托管对象，共享后对象更少。`std.gc.live/allocated`的数值会变化；接口文档已声明这些计数“可能随内部表示改变，不能依赖跨版本恒等”（`docs/标准库与内建接口.md`第813行），不改变合同。
- **Task**：跨Task边界仍由`isolated()`深复制，结构盒不跨线程共享，V1不改为非原子计数。`deep=true`的复制（`clone`、跨Task、`contains_file`拒绝）保持急切复制。
- **嵌套深度E4099**：现在在复制时检查深度。共享复制不遍历字段，所以检查改在构造时（InitField、写入嵌套结构）进行，保持同一错误码。需要确认既有用例的报错时机与位置不变；若有可观测差异，在实施前报告用户。
- **AST解释器**：`copy_value`由解释器、VM和标准库共用。新增线程局部的复制策略，VM运行期间为COW，解释器为急切复制。标准库在VM内调用时随之共享复制；需审计标准库是否原地改动传入的结构盒。初步检索显示`enum_value.cpp`、`filesystem_stdlib.cpp`等只在构造新盒时写字段，详单在实施第一步复核。
- **身份比较**：检索到的`data==`/`backing_identity`只用于Map和RecordFields内部（`collections_stdlib.cpp:51`为Map自合并判断），不依赖结构盒身份。实施时再全量复核。
- **现有局部共享机制**：`RecordFields`的共享Flat、`exposed`、`copy_depth`和`PlainRecord`在V1中保留；钉住的盒子仍走它们。V1验收后再评估是否可删除（V1c）。

## 4. 写入口清单

| 入口 | 位置 | V1处理 |
|---|---|---|
| Load结构句柄 | `src/vm.cpp` Op::Load | 写分类时对变量槽写准备并钉住 |
| Member取嵌套结构 | Op::Member、`retain_record_alias` | 写分类时在父盒字段槽写准备；读分类时只压句柄 |
| Index取元素（Slice、固定数组、Map） | Op::Index、`index_value`、`sequence_read` | 写分类时在元素槽或Map条目写准备 |
| LocateField | Op::LocateField、`field()` | 断言目标已钉住（分类保证）；未钉住时按3.2就地判定，作为保险 |
| LocateIndex、`index_location` | Op::LocateIndex | 同上（结构元素） |
| Store/StoreMulti经Location写 | `location_write` | 写入后释放钉住 |
| `mut`实参 | `enter_function`→`enforce_borrow` | 借用句柄钉住 |
| 可变方法接收者 | Op::Member生成`Callable{…,receiver}` | 接收者钉住 |
| Bind/Store/实参/返回/unwrap/标准库取出 | `copy_value`调用点 | 未钉住时共享复制 |
| 构造（MakeStruct、InitField、enum初始化） | `initialize_field`、`enum_value.cpp` | 新盒独占，不变 |
| 闭包捕获 | Op::Closure | `read_only(copy_value(...))`，即只读共享 |
| 解释器`locate` | `src/interpreter.cpp` | 急切复制策略下不变 |

## 5. 测试矩阵（新增`tests/struct_cow_tests.py`，三路径）

1. **复制后各自独立**：`var y = x`、实参、返回、unwrap、`list.get`、Map/Slice取出，之后分别改两边，每一侧都看不到对方的修改。
2. **嵌套写入**：`a.b.c = v`在`a`被共享后只改`a`；`m[k].x`、`s[i].x`、固定数组元素中的结构。
3. **右侧求值期间共享**：`x.f = g(x)`、`x.f = h()`（h读或改全局x）、多重赋值`x.a, y = …, x`，与改前二进制逐字比较输出。
4. **借用**：`mut`参数原地修改；`f(x, mut x)`；被调者经全局读同一变量；方法值保存后再改原结构。
5. **能力**：只读来源复制后改值字段成功，改共享成员仍E4007；`let`与默认参数仍E3003/E4007；`fields_readonly`经多层传播。
6. **enum**：标签与活动分支字段的共享与写入。
7. **生命周期**：异常、提前返回、defer中途释放钉住；Task内跨yield；GC多轮live稳定（不比较绝对值）。
8. **深度**：E4099的错误码、位置和时机与改前一致。
9. **全量回归**：CTest、三路径、ER0；与改前二进制对比所有`perf/bench`与`perf/stdlib`输出。

## 6. 实施切片

| 切片 | 内容 | 放行条件 |
|---|---|---|
| V1a | 加入写准备、钉住、`fields_readonly`、加载时分类和复制策略开关，但**共享复制先关闭**（copy_value行为不变） | 全部测试通过；core/stdlib A/B在噪声内（检查新增开销不超过约2%）；C2逐字节不变 |
| V1b | 打开共享复制（只在VM内，未钉住时） | 新增COW测试通过；真实编译有可重复提速；计数版的`managed_allocations`、`gc_records`和GC扫描对象数不增加；core/stdlib无确认退化；C2逐字节不变；CTest无新增失败 |
| V1c（可选） | 评估删除`RecordFields`共享Flat/`exposed`/`copy_depth`和`PlainRecord`中被COW取代的部分 | 单独剖析证明不再命中，或有收益才做 |

每个切片冻结基线后实施，结果记入性能文档S2-2a之后的新小节与`.codex-sync`运行记录。

## 7. 预期（预测，非承诺）

- 消除的工作：大部分1.74M次结构复制和对应的结构盒分配（真实编译中复制包含式约28%、结构盒分配约15.5%，两者有重叠），以及由此减少的GC扫描（约9%）。被共享后又写入的结构仍要复制一次。按写入只占复制的十分之一推算，真实编译预计1.3–1.6x，struct传值基准2x以上。
- V1b后重新剖析，再决定V2（类型ID）与V3（GC跟踪范围）的先后。

## 8. 待用户审定

| 编号 | 问题 | 建议 |
|---|---|---|
| V1-D1 | 是否采用“写路径＋钉住”的模型（第3节），而不是“写入时按路径重新解析目标”的模型。后者更接近Go，但会改变`x.f = g()`在g重绑x时的落点等边界行为 | 采用钉住模型，现行边界行为不变 |
| V1-D2 | AST解释器保持急切复制，只在VM内启用COW | 同意（解释器作为独立对照） |
| V1-D3 | 分两步实施：V1a先关闭共享验证框架开销，V1b再打开 | 同意 |
| V1-D4 | E4099若无法在构造时完全复现原时机：报告用户再定，还是接受“同错误码、时机提前到构造” | 报告用户再定 |
