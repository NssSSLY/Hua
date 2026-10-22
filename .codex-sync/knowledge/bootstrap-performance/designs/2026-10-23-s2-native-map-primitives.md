# S2接口提案：原生map集合原语（待用户确认，2026-10-23）

状态：提案，未实施。用户已接受S1的两处差异，并要求S2先交接口提案、确认后再实施。本稿依据S1之后的完整输入剖析（`build/bo11-s1-20261023/commands/full-new.stderr`）和现有标准库实现方式。

## 1. 要解决什么

能力分析中有三个helper是纯粹的map集合循环，VM按元素逐条执行：

| helper | 循环含义 | S1后完整输入独占步骤 | 每次调用 |
| --- | --- | --- | --- |
| `merge_view` | `out.origins[k] = b[k] \|\| out[k]`（并集，标志取或） | 103.4M（12.1%） | 395.7 |
| `same_flags` | 两个`map[string]bool`键与值完全相同 | 68.2M（7.9%） | 140.0 |
| `same_storage` | 两个来源集合是否有公共键 | 26.4M（3.1%） | 35.0 |
| `instantiate_view`中的内循环 | `out.origins[k] = f[k] \|\| readonly \|\| out[k]` | 未单独计量（该helper共212.6M） | — |

Hua VM每条指令计1步，标准库调用无论处理多少元素都只计常数步（与`json.encode`、`std.alg`相同）。原语化后：

- 可确定的节省：前三项合计198.0M中约178M（按改写后每次调用剩约10–30步估算），能力阶段约-21%。
- 可能的追加节省：`instantiate_view`内循环，0–约60M，需实施前先单独计量。
- 预计结果：完整输入能力阶段约8.58亿→6.2–6.8亿步。**S2单独不能把完整输入带进5亿**，之后仍需S3；这里如实写明，供确认时考虑。

## 2. 推荐接口

### 2.1 模块：新建`std.maps`

- 理由：这些是现有map类型上的通用操作，不引入新类型；名称与Go的`maps`包对应，用户易懂。标准库路线S1c候选`std.collections`计划的是Set/Deque/Heap等新类型，与本提案不冲突；将来若有Set类型，可以复用同一实现。
- 备选：放入`std.alg`（现有序列算法模块，语义不贴切），或直接放入候选`std.collections`（会提前冻结该模块的名字与范围）。

### 2.2 函数（全部为纯函数，不修改参数）

| 函数 | 签名 | 语义 | 复杂度 |
| --- | --- | --- | --- |
| `maps.equal` | `equal(a map[string]bool, b map[string]bool) bool` | 键集合相同且每个键的值相同 | O(n)，两侧都是有序树，线性同步遍历 |
| `maps.intersects` | `intersects(a map[string]bool, b map[string]bool) bool` | 至少有一个键同时存在于两边；只比较键，不看值 | O(min(n,m)·log max(n,m)) |
| `maps.union_or` | `union_or(a map[string]bool, b map[string]bool) map[string]bool` | 返回新map：键为两边并集，值为`a[k] \|\| b[k]`（缺失视为false） | O(n+m)，有序合并 |
| `maps.fill`（可选） | `fill(a map[string]bool, value bool) map[string]bool` | 返回新map：键同`a`，所有值为`value` | O(n) |

`fill`只为表达`instantiate_view`内循环中“readonly时整体取true”的情形：`union_or(out, readonly ? fill(f,true) : f)`。若不想增加接口面，可以去掉它，该内循环保持现状（少拿那部分收益）。

### 2.3 首批类型范围（需确认）

- **推荐A：首批只支持`map[string]bool`。** 编译器实际需要的就是这一种；native sema和Hua的types.hua都只需按固定签名检查，不需要新增泛型推断。名字保持通用，以后放宽时不必改名。
- 备选B：泛型`map[K]V`（K为bool/int/string；`equal`的V为bool/int/string，`union_or`/`fill`的V为bool）。更像完整的通用库，但要在native sema与selfhost/types.hua各加一套泛型签名推断，工作量和验证量约翻倍。

## 3. 语义细节

- **值语义与共享**：map在Hua中是共享容器。四个函数都不修改输入；`union_or`/`fill`返回新的、可写的独立map（`managed<MapData>()`分配，与`make_map`一致，受GC管理）。输入可以是只读视图（只读共享嵌套容器可读不可写）。
- **能力效果**：`mutates_first=false`；`union_or`/`fill`结果视为新分配、可写（`writable_result=true`，与`list.from_slice`相同）。native sema与Hua能力分析都把它当作普通纯标准调用处理，不需要新的效果规则。
- **错误**：参数类型不符时静态报E3004，动态类型检查报E4003（与现有标准函数一致）。正常输入不会失败，所以返回值不包Result。
- **工作量上界与取消**：每次调用的元素操作数上限沿用`std.alg`的32,000,000，超出报E4099 `map work limit exceeded (32000000)`；每1024次操作做一次Task取消检查点。编译器的实际规模是每个集合几十到几百条，远低于上限。
- **预算计量**：遵循现有规则，一次标准调用只计其调用指令的步数，不新增特殊计数。宿主侧工作计入墙钟时间，仍受600秒正式门槛约束。这一点要写进标准库文档的成本说明，不能当作没有代价。

## 4. 两执行器一致性与产物

- **AST解释器、源码VM、HUAB**：三者都经同一个`invoke_standard`分派到同一份C++实现，所以行为天然一致。测试仍要按三条路径各跑一遍，比较输出。
- **HUAB格式9**：标准函数带`since`，写出器取所引用函数的最大`since`作为格式号，读取器拒绝高于文件格式的函数。新函数设`since=9`，C++读写器接受格式9；旧运行时读到格式9会明确报“不支持的格式”。
- **Hua编译器自身（C07必需）**：`selfhost/archive.hua`的`required_format`目前只认6/7，必须改为按引用的标准函数计算格式（至少识别`std.maps`→9），否则自举生成的编译器会写出与内容不符的格式号，被读取器拒绝。`selfhost/signatures.hua`需增加4条签名。附带发现：该函数也不认File类型对应的格式8，这是既有缺口，本提案不顺带修，只登记。

## 5. 编译器侧改写（实施时）

- `merge_view`：`out.origins=maps.union_or(a.origins,b.origins)`，并去掉对`a`的整份clone（只复制标志字段）。
- `same_flags`：函数体改为`return maps.equal(a,b)`。
- `same_storage`：函数体改为`return maps.intersects(a.origins,b.origins)`。
- `instantiate_view`内循环：先单独计量其步骤，再决定是否用`union_or`/`fill`改写。
- 这是纯重构：能力事实应与S1基线逐字节相同（不是“近似相同”）。

## 6. 验收

1. 原语单测：空map、互不相交、部分重叠、完全相同、值不同、只读输入、大map（接近上限）、超出工作上限、Task取消；三条执行路径输出一致。
2. 类型检查：错误参数类型在native与Hua两侧都报E3004。
3. HUAB：格式9读写往返；旧格式产物不变；Hua `archive.hua`对引用`std.maps`的程序写出格式9。
4. 编译器：C04完整套件；185输入报告与S1基线逐字节相同；实例化/元数据隔离；5个真实输入HUAB与S1基线逐字节相同（不用`std.maps`的程序不应受影响）；capabilities.hua能力事实规范化导出与S1基线逐字节相同。
5. 性能：同一输入（capabilities.hua与14模块）前后对照能力阶段步骤与墙钟。
6. 文档：`docs/标准库与内建接口.md`（新模块、签名、成本说明）、`docs/标准库设计与实现路线.md`（状态）、`docs/运行时与扩展设计.md`（HUAB格式9）、过程文档与项目记忆。

## 7. 需要你确认的点

1. 模块名：`std.maps`（推荐），还是放入`std.alg`或`std.collections`？
2. 函数集合：`equal`、`intersects`、`union_or`三个必选；`fill`要不要？
3. 类型范围：首批只支持`map[string]bool`（推荐A），还是直接做泛型（B）？
4. HUAB：新增格式9（推荐），并同步修改`selfhost/archive.hua`的格式计算。
5. 预期如实：S2之后完整输入能力阶段预计仍有约6.2–6.8亿步，之后还要做S3。
