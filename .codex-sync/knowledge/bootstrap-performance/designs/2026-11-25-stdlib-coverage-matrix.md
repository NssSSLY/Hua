# Hua 标准库逐接口多路径覆盖表（2026-11-25 源码实核）

生成工具：`python tests/tools/audit_stdlib_coverage.py --repo .`（直接解析 `src/stdlib.cpp`、`selfhost/catalog.hua`、`selfhost/signatures.hua`、`selfhost/types.hua`、`selfhost/archive.hua` 与 `selfhost/native_cpp.hua`）。

## 1. 汇总统计

| 指标 | 数量 | 说明 |
| --- | --- | --- |
| 宿主内建标准模块 / 函数 | 27 模块 / 374 函数 | `src/stdlib.cpp` 权威注册表（`HUAB since 3–29`） |
| SDK 纯 Hua 源码标准模块 | 1 模块 / 1 函数 | `std.alg_integer.lower_bound`（Host / SH-Check / SH-HUAB / Release-EXE 四路径已全通，HUAB 6） |
| 公开全局核心内建 | 38 个 | `catalog.core` 全部已登记；`native_cpp.hua` 当前直接支持 `print/str/int/max/len/clone/has/delete/ok/err/is_ok/is_err/unwrap/unwrap_err/unwrap_or/is_some/is_none`（17 个） |
| `Host`（解释器 / 源码 VM / 宿主 HUAB） | 374 / 374 | 全部 374 个函数在宿主三路径均已实现并验收 |
| `SH-Check`（自举名称 + 类型能力检查） | 206 / 374 | `catalog.hua` 14 模块共导出 137 个名称（134 个函数名 + 3 个类型名 `Value`/`File`/`IOFailure`），134 个函数均已在 `signatures.hua` 登记 |
| `SH-HUAB`（自举归档编译器 `hua-archive.exe`） | 206 / 374 | `archive.hua` 已对 `fs.canonical`(17) 与 `file.read_line` 等 4 项(29) 升级 `load_format`；其余已登记项均满足 `since <= 8` |
| `Release-EXE`（`hua build --release` 原生生成） | 185 / 374 | `native_cpp.hua` 当前桥接 48 项 `std.*` + 内联 13 项 `std.*` = **61 项**（注：`std.file` 16 项虽已进 `catalog/archive`，但 `native_cpp.hua` 尚未映射 `FileTag`/`IOFailureTag`） |

### 按改造机制分类（为什么不能按函数个数简单估工期）

| 缺口类别 | 函数数 | 工程本质与所需动作 |
| --- | --- | --- |
| A · 已全路径打通 | 185 | Host、自举检查、自举 HUAB、`hua build --release` 四路径全部就绪。 |
| B · 仅差纯表项/桥接登记 | 101 | 参数与返回类型在 `types.hua` 和 `native_cpp.hua::cpp_type` 中**均已支持**（且 C++ `src/*_stdlib.cpp` 实现已在 `libhua_semantics.a` 中）；只需补齐 `catalog` / `signatures` / `archive(since)` / `native_cpp::bridged()`（`std.math` 另需放开 `types.hua:335` 仅 `float` 检查）。 |
| C · 需 Task 原生/自举特化 | 9 | `std.task` 在 Host 与自举 HUAB 已通；原生 `--release` 走并发纤程特化路径。 |
| C · 需补不透明句柄类型映射 | 5 | `include/hua/native_rt.hpp` **已经定义好** `FileTag`、`IOFailureTag`、`ErrorTag`、`Rng`；只需在 `types.hua::typename_context` 与 `native_cpp.hua::cpp_type` 补 4 个类型分支并加入 `bridged()`，即可一次打通 `std.file`(16) + `std.error`(16) + `std.random`(5)。 |
| D · 需多态签名规则 | 41 | 签名含泛型或开放参数（`""`，如 `json.from`/`encode_pretty`、`os.parse_args`/`environ`、`maps`/`sets`/`list` 新增项、`alg` 非回调项、`test.equal`）；需在 `types.hua::standard()` 补类型推导分支。 |
| D+C · 需多态规则+类型映射 | 14 | `std.deque`(11) 与 `std.random.shuffle/choice/sample`(3)：需同时补 `Deque<T>`/`Rng` 类型映射与 `types.hua` 元素类型推导。 |
| E · 需高阶回调支持 | 19 | L0-1 高阶函数（19 项：`alg.map/filter/fold/...`、`list.sort_by/...`、`heap.*_by_key`、`maps.update`）。`native_algorithms.hpp` 已有 C++ 模板实现，但 `selfhost` 需补函数类型参数检查与回调代码发射。 |

## 2. 模块级覆盖总览

| 模块 | 总数 | Host | SH-Check | SH-HUAB | Release-EXE | 最低版本范围 | 首批 CLI 样例相关性 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `std.alg` | 27 | 27 | 10 | 10 | 10 | HUAB 6–20 | 基础 3/27 全通、10/27 SH 已通（够首批 CLI 使用） |
| `std.buffer` | 14 | 14 | 14 | 14 | 14 | HUAB 4–9 | 已基本全通（7/14 全通，余 7 项含 `write_str`/`take_text` 仅差登记+`bridged()`） |
| `std.bytes` | 13 | 13 | 13 | 13 | 13 | HUAB 4 | 已基本全通（8/13 全通，余 5 项仅差 `bridged()`） |
| `std.crypto` | 5 | 5 | 0 | 0 | 0 | HUAB 13 | 后续批次按需对齐 |
| `std.deque` | 11 | 11 | 0 | 0 | 0 | HUAB 14 | 后续批次按需对齐 |
| `std.encoding` | 17 | 17 | 0 | 0 | 0 | HUAB 12–24 | 后续批次按需对齐 |
| `std.error` | 16 | 16 | 16 | 16 | 16 | HUAB 7 | **核心必需**（`native_rt.hpp` 已有 `ErrorTag`；补类型映射即一次打通全部 16 项结构化错误） |
| `std.file` | 16 | 16 | 16 | 16 | 16 | HUAB 8–29 | **核心必需**（SH-HUAB 16 项已通；`native_cpp.hua` 补 `FileTag`/`IOFailureTag` 即一次打通全部 16 项含 `read_line`） |
| `std.fs` | 19 | 19 | 19 | 19 | 19 | HUAB 3–17 | **核心必需**（7 项已通；`exists`/`mkdir_all`/`list_dir` 仅差 `bridged()`，可选补 `walk`/`copy_file`） |
| `std.gc` | 4 | 4 | 0 | 0 | 0 | HUAB 6 | 后续批次按需对齐 |
| `std.heap` | 8 | 8 | 0 | 0 | 0 | HUAB 14 | 后续批次按需对齐 |
| `std.io` | 4 | 4 | 4 | 4 | 4 | HUAB 3–18 | **高**（补 `bridged()` 打通 `read_line`/`read_all`/`write`/`write_err` stderr 诊断输出） |
| `std.json` | 23 | 23 | 23 | 23 | 23 | HUAB 3–27 | **核心必需**（6 项全通、6 项缺 `bridged()`；建议首批补 `decode_lines`/`encode_lines`/`object`/`array`/`set`/`from`/`encode_pretty`） |
| `std.list` | 23 | 23 | 10 | 10 | 10 | HUAB 4–20 | 基础 9/23 已全通（够首批 CLI 使用） |
| `std.log` | 6 | 6 | 0 | 0 | 0 | HUAB 18 | 后续批次按需对齐 |
| `std.maps` | 7 | 7 | 0 | 0 | 0 | HUAB 10–11 | 后续批次按需对齐 |
| `std.math` | 53 | 53 | 12 | 12 | 0 | HUAB 6–22 | 后续批次按需对齐 |
| `std.os` | 10 | 10 | 8 | 8 | 8 | HUAB 3–28 | **核心必需**（`args`/`cwd`/`getenv` 已通；建议首批补 `parse_args`、`exit`） |
| `std.path` | 6 | 6 | 6 | 6 | 6 | HUAB 6 | **核心必需**（6/6 已全路径打通） |
| `std.process` | 2 | 2 | 0 | 0 | 0 | HUAB 28 | 后续批次按需对齐 |
| `std.random` | 8 | 8 | 0 | 0 | 0 | HUAB 13 | 后续批次按需对齐 |
| `std.sets` | 7 | 7 | 0 | 0 | 0 | HUAB 10–21 | 后续批次按需对齐 |
| `std.simd` | 4 | 4 | 0 | 0 | 0 | HUAB 6 | 后续批次按需对齐 |
| `std.strings` | 44 | 44 | 44 | 44 | 44 | HUAB 3–26 | **核心必需**（13 项已通；建议首批补 `trim_*`/`lines`/`fields`/`parse_int`/`parse_float`/`format` 等 B 类项） |
| `std.task` | 9 | 9 | 9 | 9 | 0 | HUAB 6 | 后续批次按需对齐 |
| `std.test` | 4 | 4 | 0 | 0 | 0 | HUAB 18 | 后续批次按需对齐 |
| `std.time` | 14 | 14 | 2 | 2 | 2 | HUAB 6–15 | 后续批次按需对齐 |
| `std.alg_integer` *(SDK 源码)* | 1 | 1 | 1 | 1 | 1 | HUAB 6 | 纯 Hua 标准模块样板（四路径已通） |

## 3. 逐接口覆盖明细表（27 内建模块 374 函数 + 1 源码模块）

路径图例：`✓` 已支持，`✗` 未接通。所有 `Host` 列均涵盖 `hua interpret`、源码 `hua run`、宿主 `hua build` + 无源码 `hua run <.huab>`。

| 序号 | 接口 | 签名 | 最低版本 | Host | SH-Check | SH-HUAB | Release-EXE | 缺口分类 | 成功或失败验证用例 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `std.alg.unique` | `(xs []T) []T` | HUAB 19 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `alg.unique(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 2 | `std.alg.chunk` | `(xs []T, size int) [][]T` | HUAB 19 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `alg.chunk(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 3 | `std.alg.product` | `(xs []T) Result<T,string> [T: int\|float]` | HUAB 19 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `alg.product(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 4 | `std.alg.sorted` | `(xs []T) Result<[]T,string> [T: int\|float\|string]` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.sorted(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 5 | `std.alg.reverse` | `(xs []T) Result<[]T,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.reverse(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 6 | `std.alg.contains` | `(xs []T, target T) bool` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.contains(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 7 | `std.alg.index` | `(xs []T, target T) int` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.index(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 8 | `std.alg.lower_bound` | `(xs []T, target T) int` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.lower_bound(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 9 | `std.alg.upper_bound` | `(xs []T, target T) int` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.upper_bound(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 10 | `std.alg.binary_search` | `(xs []T, target T) int?` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.binary_search(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 11 | `std.alg.sum` | `(xs []T) Result<T,string> [T: int\|float]` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.sum(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 12 | `std.alg.min` | `(xs []T) Result<T,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.min(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 13 | `std.alg.max` | `(xs []T) Result<T,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `alg.max(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 14 | `std.alg.map` | `(xs []T, f fn(T)->U) []U` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.map(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 15 | `std.alg.fold` | `(xs []T, init A, f fn(A,T)->A) A` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.fold(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 16 | `std.alg.filter` | `(xs []T, pred fn(T)->bool) []T` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.filter(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 17 | `std.alg.any` | `(xs []T, pred fn(T)->bool) bool` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.any(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 18 | `std.alg.all` | `(xs []T, pred fn(T)->bool) bool` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.all(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 19 | `std.alg.find` | `(xs []T, pred fn(T)->bool) T?` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.find(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 20 | `std.alg.find_index` | `(xs []T, pred fn(T)->bool) int` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.find_index(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 21 | `std.alg.sorted_by_key` | `(xs []T, key fn(T)->K) []T` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.sorted_by_key(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 22 | `std.alg.sorted_by` | `(xs []T, less fn(T,T)->bool) []T` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.sorted_by(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 23 | `std.alg.min_by_key` | `(xs []T, key fn(T)->K) Result<T,string>` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.min_by_key(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 24 | `std.alg.max_by_key` | `(xs []T, key fn(T)->K) Result<T,string>` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.max_by_key(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 25 | `std.alg.partition` | `(xs []T, pred fn(T)->bool) ([]T, []T)` | HUAB 20 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.partition(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 26 | `std.alg.zip_with` | `(xs []A, ys []B, f fn(A,B)->R) []R` | HUAB 20 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `alg.zip_with(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 27 | `std.alg.windows` | `(xs []T, size int) [][]T` | HUAB 20 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `alg.windows(...)` → 切片输入返回确定序列/下标；元素类型不符静态 `E3004` |
| 28 | `std.buffer.new` | `() Buffer [writable]` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.new(...)` → 三路径结果一致 |
| 29 | `std.buffer.len` | `(Buffer) int` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.len(...)` → 三路径结果一致 |
| 30 | `std.buffer.append` | `(mut Buffer, Bytes) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.append(...)` → 三路径结果一致 |
| 31 | `std.buffer.write_u8` | `(mut Buffer, int) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.write_u8(...)` → 三路径结果一致 |
| 32 | `std.buffer.write_u32_le` | `(mut Buffer, int) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.write_u32_le(...)` → 三路径结果一致 |
| 33 | `std.buffer.write_i64_le` | `(mut Buffer, int) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.write_i64_le(...)` → 三路径结果一致 |
| 34 | `std.buffer.write_text` | `(mut Buffer, string) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.write_text(...)` → 三路径结果一致 |
| 35 | `std.buffer.bytes` | `(Buffer) Bytes` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.bytes(...)` → 三路径结果一致 |
| 36 | `std.buffer.text` | `(Buffer) Result<string,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.text(...)` → 三路径结果一致 |
| 37 | `std.buffer.clear` | `(mut Buffer) void` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.clear(...)` → 三路径结果一致 |
| 38 | `std.buffer.write_str` | `(mut Buffer, string) void` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.write_str(...)` → 三路径结果一致 |
| 39 | `std.buffer.write_int` | `(mut Buffer, int) void` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.write_int(...)` → 三路径结果一致 |
| 40 | `std.buffer.take_text` | `(mut Buffer) Result<string,string>` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.take_text(...)` → 三路径结果一致 |
| 41 | `std.buffer.reserve` | `(mut Buffer, int) void` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `buffer.reserve(...)` → 三路径结果一致 |
| 42 | `std.bytes.from_ints` | `([]int) Result<Bytes,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.from_ints(...)` → 三路径结果一致 |
| 43 | `std.bytes.from_text` | `(string) Bytes` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.from_text(...)` → 三路径结果一致 |
| 44 | `std.bytes.to_text` | `(Bytes) Result<string,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.to_text(...)` → 三路径结果一致 |
| 45 | `std.bytes.to_ints` | `(Bytes) Result<[]int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.to_ints(...)` → 三路径结果一致 |
| 46 | `std.bytes.len` | `(Bytes) int` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.len(...)` → 三路径结果一致 |
| 47 | `std.bytes.at` | `(Bytes, int) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.at(...)` → 三路径结果一致 |
| 48 | `std.bytes.slice` | `(Bytes, int, int) Result<Bytes,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.slice(...)` → 三路径结果一致 |
| 49 | `std.bytes.concat` | `(Bytes, Bytes) Result<Bytes,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.concat(...)` → 三路径结果一致 |
| 50 | `std.bytes.u32_le` | `(int) Result<Bytes,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.u32_le(...)` → 三路径结果一致 |
| 51 | `std.bytes.i64_le` | `(int) Bytes` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.i64_le(...)` → 三路径结果一致 |
| 52 | `std.bytes.read_u32_le` | `(Bytes, int) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.read_u32_le(...)` → 三路径结果一致 |
| 53 | `std.bytes.read_i64_le` | `(Bytes, int) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.read_i64_le(...)` → 三路径结果一致 |
| 54 | `std.bytes.crc32` | `(Bytes) int` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `bytes.crc32(...)` → 三路径结果一致 |
| 55 | `std.crypto.random_bytes` | `(int) Result<Bytes,string>` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `crypto.random_bytes(...)` → 正常返回随机/摘要结果；负数长度返回 `Err` |
| 56 | `std.crypto.uuid4` | `() Result<string,string>` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `crypto.uuid4(...)` → 正常返回随机/摘要结果；负数长度返回 `Err` |
| 57 | `std.crypto.sha256` | `(Bytes) Bytes` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.hex_encode(crypto.sha256(bytes.from_text("abc")))` → `"ba7816bf..."` |
| 58 | `std.crypto.hmac_sha256` | `(Bytes, Bytes) Bytes` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `crypto.hmac_sha256(...)` → 正常返回随机/摘要结果；负数长度返回 `Err` |
| 59 | `std.crypto.equal` | `(Bytes, Bytes) bool` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `crypto.equal(...)` → 正常返回随机/摘要结果；负数长度返回 `Err` |
| 60 | `std.deque.from_slice` | `(xs []T) Deque<T> [writable]` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.from_slice(...)` → 返回预期容器结果 |
| 61 | `std.deque.len` | `(d Deque<T>) int` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.len(...)` → 返回预期容器结果 |
| 62 | `std.deque.push_back` | `(mut d Deque<T>, value T) Result<int,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.push_back(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 63 | `std.deque.push_front` | `(mut d Deque<T>, value T) Result<int,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.push_front(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 64 | `std.deque.pop_back` | `(mut d Deque<T>) Result<T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.pop_back(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 65 | `std.deque.pop_front` | `(mut d Deque<T>) Result<T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.pop_front(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 66 | `std.deque.front` | `(d Deque<T>) Result<T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.front(...)` → 返回预期容器结果 |
| 67 | `std.deque.back` | `(d Deque<T>) Result<T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.back(...)` → 返回预期容器结果 |
| 68 | `std.deque.get` | `(d Deque<T>, index int) Result<T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.get(...)` → 返回预期容器结果 |
| 69 | `std.deque.clear` | `(mut d Deque<T>) void` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.clear(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 70 | `std.deque.snapshot` | `(d Deque<T>) []T` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `deque.snapshot(...)` → 返回预期容器结果 |
| 71 | `std.encoding.hex_encode` | `(Bytes) string` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.hex_encode(...)` → 纯函数返回编码文本或布尔校验值 |
| 72 | `std.encoding.hex_decode` | `(string) Result<Bytes,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.hex_decode(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 73 | `std.encoding.base64_encode` | `(Bytes) string` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.base64_encode(...)` → 纯函数返回编码文本或布尔校验值 |
| 74 | `std.encoding.base64_decode` | `(string) Result<Bytes,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.base64_decode(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 75 | `std.encoding.base64url_encode` | `(Bytes) string` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.base64url_encode(...)` → 纯函数返回编码文本或布尔校验值 |
| 76 | `std.encoding.base64url_decode` | `(string) Result<Bytes,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.base64url_decode(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 77 | `std.encoding.utf8_valid` | `(Bytes) bool` | HUAB 23 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.utf8_valid(...)` → 纯函数返回编码文本或布尔校验值 |
| 78 | `std.encoding.utf8_encode` | `(int) Result<Bytes,string>` | HUAB 23 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.utf8_encode(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 79 | `std.encoding.utf8_decode` | `(Bytes, int) (int,int)` | HUAB 23 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.utf8_decode(...)` → 纯函数返回编码文本或布尔校验值 |
| 80 | `std.encoding.pack_uint` | `(int, int, string) Result<Bytes,string>` | HUAB 24 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.pack_uint(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 81 | `std.encoding.pack_int` | `(int, int, string) Result<Bytes,string>` | HUAB 24 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.pack_int(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 82 | `std.encoding.unpack_uint` | `(Bytes, int, int, string) Result<int,string>` | HUAB 24 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.unpack_uint(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 83 | `std.encoding.unpack_int` | `(Bytes, int, int, string) Result<int,string>` | HUAB 24 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.unpack_int(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 84 | `std.encoding.pack_f32` | `(float, string) Result<Bytes,string>` | HUAB 24 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.pack_f32(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 85 | `std.encoding.pack_f64` | `(float, string) Result<Bytes,string>` | HUAB 24 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.pack_f64(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 86 | `std.encoding.unpack_f32` | `(Bytes, int, string) Result<float,string>` | HUAB 24 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.unpack_f32(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 87 | `std.encoding.unpack_f64` | `(Bytes, int, string) Result<float,string>` | HUAB 24 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `encoding.unpack_f64(...)` → 合法编码往返一致；非法字节/端序名返回 `Err` |
| 88 | `std.error.make` | `(string, string, string) Result<std.error.Value,string>` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.make("app.cfg", "bad_field", "missing id")` → `ok(Error)`；空 domain 返回 `Err` |
| 89 | `std.error.wrap` | `(string, string, string, std.error.Value) Result<std.error.Value,string>` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.wrap(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 90 | `std.error.from_string` | `(string, string) std.error.Value` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.from_string(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 91 | `std.error.with_context` | `(std.error.Value, string) std.error.Value` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.with_context(e, "line 3")` → 返回附加上下文的新 `Error` |
| 92 | `std.error.with_origin` | `(std.error.Value, string) std.error.Value` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.with_origin(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 93 | `std.error.with_native` | `(std.error.Value, string, int) std.error.Value` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.with_native(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 94 | `std.error.domain` | `(std.error.Value) string` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.domain(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 95 | `std.error.code` | `(std.error.Value) string` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.code(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 96 | `std.error.message` | `(std.error.Value) string` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.message(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 97 | `std.error.cause` | `(std.error.Value) std.error.Value?` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.cause(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 98 | `std.error.origin` | `(std.error.Value) string?` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.origin(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 99 | `std.error.native_domain` | `(std.error.Value) string?` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.native_domain(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 100 | `std.error.native_code` | `(std.error.Value) int?` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.native_code(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 101 | `std.error.contexts` | `(std.error.Value) []string` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.contexts(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 102 | `std.error.truncated` | `(std.error.Value) bool` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.truncated(...)` → 校验或读取 `std.error.Value` 字段，非法参数返回 `Err`/空可选值 |
| 103 | `std.error.format` | `(std.error.Value) string` | HUAB 7 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `error.format(e)` → `"app.cfg/bad_field: missing id"` |
| 104 | `std.file.open` | `(string, string) Result<std.file.File,std.error.Value>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.open("missing.txt","r")` → `Err(std.error.Value)` (`code=="not_found"`) |
| 105 | `std.file.read` | `(std.file.File, int) Result<Bytes?,std.file.IOFailure>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.read(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 106 | `std.file.write` | `(std.file.File, Bytes) Result<int,std.file.IOFailure>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.write(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 107 | `std.file.seek` | `(std.file.File, int, string) Result<int,std.error.Value>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.seek(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 108 | `std.file.tell` | `(std.file.File) Result<int,std.error.Value>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.tell(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 109 | `std.file.flush` | `(std.file.File) Result<void,std.error.Value>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.flush(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 110 | `std.file.close` | `(std.file.File) Result<void,std.error.Value>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.close(f)` → 首次 `ok()`，重复 `close` 幂等 `ok()` |
| 111 | `std.file.state` | `(std.file.File) Result<string,std.error.Value>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.state(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 112 | `std.file.close_error` | `(std.file.File) Result<std.error.Value?,std.error.Value>` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.close_error(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 113 | `std.file.error` | `(std.file.IOFailure) std.error.Value` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.error(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 114 | `std.file.transferred` | `(std.file.IOFailure) int?` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.transferred(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 115 | `std.file.partial` | `(std.file.IOFailure) Bytes?` | HUAB 8 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.partial(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 116 | `std.file.read_line` | `(std.file.File) Result<string?,std.file.IOFailure>` | HUAB 29 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.read_line(f)` → 逐行去 `\r\n` 返回 `ok("...")`，EOF 返回 `ok(nil)` |
| 117 | `std.file.read_all` | `(std.file.File) Result<Bytes,std.file.IOFailure>` | HUAB 29 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.read_all(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 118 | `std.file.read_exact` | `(std.file.File, int) Result<Bytes?,std.file.IOFailure>` | HUAB 29 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.read_exact(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 119 | `std.file.write_all` | `(std.file.File, Bytes) Result<void,std.file.IOFailure>` | HUAB 29 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `file.write_all(...)` → 打开态正常执行；已 `close` 句柄返回 `closed` 错误 |
| 120 | `std.fs.stat` | `(string) Result<Json,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.stat(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 121 | `std.fs.rename` | `(string, string) Result<bool,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.rename(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 122 | `std.fs.replace` | `(string, string) Result<bool,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.replace(tmp, dst)` → 同卷原子替换 `ok(true)` |
| 123 | `std.fs.temp_file` | `(string, string) Result<string,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.temp_file(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 124 | `std.fs.walk` | `(string) Result<[]string,string>` | HUAB 17 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.walk(dir)` → `ok(字典序相对路径切片)`；非目录返回 `Err("FS_TYPE...")` |
| 125 | `std.fs.copy_file` | `(string, string) Result<bool,string>` | HUAB 17 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.copy_file(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 126 | `std.fs.copy_dir` | `(string, string) Result<int,string>` | HUAB 17 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.copy_dir(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 127 | `std.fs.remove_all` | `(string) Result<int,string>` | HUAB 17 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.remove_all(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 128 | `std.fs.canonical` | `(string) Result<string,string>` | HUAB 17 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.canonical(".")` → `ok(规范绝对路径)`；不存在返回 `Err("FS_NOT_FOUND...")` |
| 129 | `std.fs.read_text` | `(string) Result<string,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.read_text("config.json")` → `ok(text)`；不存在返回 `Err` |
| 130 | `std.fs.write_text` | `(string, string) Result<bool,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.write_text(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 131 | `std.fs.append_text` | `(string, string) Result<bool,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.append_text(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 132 | `std.fs.exists` | `(string) Result<bool,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.exists(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 133 | `std.fs.remove` | `(string) Result<bool,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.remove(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 134 | `std.fs.mkdir_all` | `(string) Result<bool,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.mkdir_all(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 135 | `std.fs.list_dir` | `(string) Result<[]string,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.list_dir(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 136 | `std.fs.read_bytes` | `(string) Result<Bytes,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.read_bytes(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 137 | `std.fs.write_bytes` | `(string, Bytes) Result<bool,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.write_bytes(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 138 | `std.fs.append_bytes` | `(string, Bytes) Result<bool,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `fs.append_bytes(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 139 | `std.gc.collect` | `() int` | HUAB 6 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `gc.collect()` → 返回非负托管对象统计计数 |
| 140 | `std.gc.live` | `() int` | HUAB 6 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `gc.live()` → 返回非负托管对象统计计数 |
| 141 | `std.gc.allocated` | `() int` | HUAB 6 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `gc.allocated()` → 返回非负托管对象统计计数 |
| 142 | `std.gc.collected` | `() int` | HUAB 6 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `gc.collected()` → 返回非负托管对象统计计数 |
| 143 | `std.heap.heapify` | `(mut xs List<T>) void` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `heap.heapify(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 144 | `std.heap.push` | `(mut xs List<T>, value T) Result<int,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `heap.push(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 145 | `std.heap.pop` | `(mut xs List<T>) Result<T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `heap.pop(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 146 | `std.heap.peek` | `(xs List<T>) Result<T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `heap.peek(...)` → 返回预期容器结果 |
| 147 | `std.heap.smallest` | `(xs []T, k int) Result<[]T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `heap.smallest(...)` → 返回预期容器结果 |
| 148 | `std.heap.largest` | `(xs []T, k int) Result<[]T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `heap.largest(...)` → 返回预期容器结果 |
| 149 | `std.heap.push_by_key` | `(mut xs List<T>, value T, key fn(T)->K) Result<int,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `heap.push_by_key(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 150 | `std.heap.pop_by_key` | `(mut xs List<T>, key fn(T)->K) Result<T,string>` | HUAB 14 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `heap.pop_by_key(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 151 | `std.io.write_err` | `(string) Result<int,string>` | HUAB 18 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `io.write_err("warn\n")` → 写 stderr 并返回 `ok(5)` |
| 152 | `std.io.read_line` | `() Result<string?,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `io.read_line(...)` → 读写标准流返回 `ok(...)`；超 16 MiB 返回 `Err` |
| 153 | `std.io.read_all` | `() Result<string,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `io.read_all(...)` → 读写标准流返回 `ok(...)`；超 16 MiB 返回 `Err` |
| 154 | `std.io.write` | `(string) Result<int,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `io.write(...)` → 读写标准流返回 `ok(...)`；超 16 MiB 返回 `Err` |
| 155 | `std.json.decode` | `(string) Result<Json,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.decode("{\"a\":1}")` → `ok(Json)`；坏语法返回 `Err` |
| 156 | `std.json.encode` | `(value any_json_serializable) Result<string,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.encode(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 157 | `std.json.kind` | `(Json) string` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.kind(...)` → 构造或查询不可变 `Json` 节点 |
| 158 | `std.json.get` | `(Json, string) Result<Json,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.get(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 159 | `std.json.at` | `(Json, int) Result<Json,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.at(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 160 | `std.json.len` | `(Json) Result<int,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.len(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 161 | `std.json.keys` | `(Json) Result<[]string,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.keys(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 162 | `std.json.as_string` | `(Json) Result<string,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.as_string(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 163 | `std.json.as_int` | `(Json) Result<int,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.as_int(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 164 | `std.json.as_float` | `(Json) Result<float,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.as_float(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 165 | `std.json.as_bool` | `(Json) Result<bool,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.as_bool(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 166 | `std.json.is_null` | `(Json) bool` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.is_null(...)` → 构造或查询不可变 `Json` 节点 |
| 167 | `std.json.null` | `() Json` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.null(...)` → 构造或查询不可变 `Json` 节点 |
| 168 | `std.json.from` | `(value any_json_serializable) Result<Json,string>` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.from(map[string]int{"k":1})` → `ok(Json)` |
| 169 | `std.json.array` | `([]Json) Json` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.array(...)` → 构造或查询不可变 `Json` 节点 |
| 170 | `std.json.object` | `(map<string,Json>) Json` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.object(map[string]Json{"ok":unwrap(json.from(true))})` → `Json` |
| 171 | `std.json.set` | `(Json, string, Json) Result<Json,string>` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.set(obj, "k", val)` → 返回新 `ok(Json)`，原 `obj` 不变；非 object 返回 `Err` |
| 172 | `std.json.remove` | `(Json, string) Result<Json,string>` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.remove(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 173 | `std.json.append` | `(Json, Json) Result<Json,string>` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.append(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 174 | `std.json.set_at` | `(Json, int, Json) Result<Json,string>` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.set_at(...)` → 类型匹配返回 `ok(...)`；种类不符或越界返回 `Err` |
| 175 | `std.json.encode_pretty` | `(value any_json_serializable, indent int) Result<string,string>` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.encode_pretty(obj, 2)` → `ok(2空格缩进JSON)`；`indent<0` 返回 `Err` |
| 176 | `std.json.decode_lines` | `(string) Result<[]Json,string>` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.decode_lines("{\"a\":1}\n{\"a\":2}\n")` → `ok(2项 []Json)`；坏行带行号 `Err` |
| 177 | `std.json.encode_lines` | `([]Json) Result<string,string>` | HUAB 27 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `json.encode_lines([j1, j2])` → `ok("{\"a\":1}\n{\"a\":2}\n")` |
| 178 | `std.list.from_slice` | `(xs []T) List<T> [writable]` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.from_slice(...)` → 返回预期容器结果 |
| 179 | `std.list.len` | `(xs List<T>) int` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.len(...)` → 返回预期容器结果 |
| 180 | `std.list.get` | `(xs List<T>, index int) Result<T,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.get(...)` → 返回预期容器结果 |
| 181 | `std.list.set` | `(mut xs List<T>, index int, value T) Result<bool,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.set(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 182 | `std.list.append` | `(mut xs List<T>, value T) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.append(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 183 | `std.list.extend` | `(mut xs List<T>, values []T) Result<int,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.extend(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 184 | `std.list.pop` | `(mut xs List<T>) Result<T,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.pop(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 185 | `std.list.snapshot` | `(xs List<T>) []T` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.snapshot(...)` → 返回预期容器结果 |
| 186 | `std.list.clear` | `(mut xs List<T>) void` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.clear(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 187 | `std.list.reserve` | `(mut xs List<T>, capacity int) Result<bool,string>` | HUAB 4 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `list.reserve(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 188 | `std.list.insert` | `(mut xs List<T>, index int, value T) Result<int,string>` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.insert(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 189 | `std.list.remove_at` | `(mut xs List<T>, index int) Result<T,string>` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.remove_at(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 190 | `std.list.index_of` | `(xs List<T>, target T) int` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.index_of(...)` → 返回预期容器结果 |
| 191 | `std.list.contains` | `(xs List<T>, target T) bool` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.contains(...)` → 返回预期容器结果 |
| 192 | `std.list.reverse` | `(mut xs List<T>) void` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.reverse(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 193 | `std.list.sort` | `(mut xs List<T>) void` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.sort(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 194 | `std.list.swap` | `(mut xs List<T>, i int, j int) Result<bool,string>` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.swap(...)` → 正常返回 `ok(...)`；空容器/越界返回 `Err`；只读容器调用静态 `E3003`/动态 `E4007` |
| 195 | `std.list.truncate` | `(mut xs List<T>, len int) void` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.truncate(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 196 | `std.list.slice` | `(xs List<T>, start int, end int) Result<[]T,string>` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.slice(...)` → 返回预期容器结果 |
| 197 | `std.list.sort_by_key` | `(mut xs List<T>, key fn(T)->K) void` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `list.sort_by_key(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 198 | `std.list.sort_by` | `(mut xs List<T>, less fn(T,T)->bool) void` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `list.sort_by(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 199 | `std.list.retain` | `(mut xs List<T>, pred fn(T)->bool) int` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `list.retain(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 200 | `std.list.dedup` | `(mut xs List<T>) int` | HUAB 20 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `list.dedup(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 201 | `std.log.debug` | `(string) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `log.debug(...)` → 按级别向 stderr 输出 logfmt 行；非法级别报 `E4003` |
| 202 | `std.log.info` | `(string) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `log.info(...)` → 按级别向 stderr 输出 logfmt 行；非法级别报 `E4003` |
| 203 | `std.log.warn` | `(string) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `log.warn(...)` → 按级别向 stderr 输出 logfmt 行；非法级别报 `E4003` |
| 204 | `std.log.error` | `(string) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `log.error(...)` → 按级别向 stderr 输出 logfmt 行；非法级别报 `E4003` |
| 205 | `std.log.with_fields` | `(string, string, map<string,string>) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `log.with_fields(...)` → 按级别向 stderr 输出 logfmt 行；非法级别报 `E4003` |
| 206 | `std.log.set_level` | `(string) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `log.set_level(...)` → 按级别向 stderr 输出 logfmt 行；非法级别报 `E4003` |
| 207 | `std.maps.keys` | `(m map[K]V) []K` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `maps.keys(...)` → 返回预期容器结果 |
| 208 | `std.maps.values` | `(m map[K]V) []V` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `maps.values(...)` → 返回预期容器结果 |
| 209 | `std.maps.entries` | `(m map[K]V) ([]K, []V)` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `maps.entries(...)` → 返回预期容器结果 |
| 210 | `std.maps.get_or` | `(m map[K]V, key K, fallback V) V` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `maps.get_or(...)` → 返回预期容器结果 |
| 211 | `std.maps.pop` | `(mut m map[K]V, key K) V?` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `maps.pop(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 212 | `std.maps.merge` | `(mut dst map[K]V, src map[K]V) int` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `maps.merge(...)` → 返回预期容器结果；只读容器调用静态 `E3003`/动态 `E4007` |
| 213 | `std.maps.update` | `(mut m map[K]V, key K, init V, f fn(V)->V) V` | HUAB 11 | ✓ | ✗ | ✗ | ✗ | E · 需高阶回调支持 | `maps.update(...)` → 正常返回转换/聚合结果；回调抛诊断或改写原容器报 `E4003` |
| 214 | `std.math.floor` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.floor(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 215 | `std.math.ceil` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.ceil(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 216 | `std.math.trunc` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.trunc(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 217 | `std.math.round` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.round(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 218 | `std.math.sin` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.sin(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 219 | `std.math.cos` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.cos(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 220 | `std.math.tan` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.tan(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 221 | `std.math.exp` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.exp(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 222 | `std.math.log` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.log(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 223 | `std.math.log2` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.log2(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 224 | `std.math.log10` | `(float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.log10(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 225 | `std.math.hypot` | `(float, float) Result<float,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | B · 仅差纯表项/桥接登记 | `math.hypot(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 226 | `std.math.gcd` | `(int, int) int` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.gcd(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 227 | `std.math.lcm` | `(int, int) int` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.lcm(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 228 | `std.math.isqrt` | `(int) int` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.isqrt(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 229 | `std.math.asin` | `(float) Result<float,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.asin(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 230 | `std.math.acos` | `(float) Result<float,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.acos(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 231 | `std.math.atan` | `(float) Result<float,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.atan(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 232 | `std.math.atan2` | `(float, float) Result<float,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.atan2(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 233 | `std.math.cbrt` | `(float) Result<float,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.cbrt(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 234 | `std.math.fmod` | `(float, float) Result<float,string>` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.fmod(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 235 | `std.math.pi` | `() float` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.pi(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 236 | `std.math.e` | `() float` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.e(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 237 | `std.math.tau` | `() float` | HUAB 12 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.tau(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 238 | `std.math.popcount` | `(int) int` | HUAB 21 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.popcount(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 239 | `std.math.leading_zeros` | `(int) int` | HUAB 21 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.leading_zeros(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 240 | `std.math.trailing_zeros` | `(int) int` | HUAB 21 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.trailing_zeros(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 241 | `std.math.rotate_left` | `(int, int) int` | HUAB 21 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.rotate_left(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 242 | `std.math.rotate_right` | `(int, int) int` | HUAB 21 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.rotate_right(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 243 | `std.math.checked_add` | `(int, int) Result<int,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.checked_add(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 244 | `std.math.checked_sub` | `(int, int) Result<int,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.checked_sub(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 245 | `std.math.checked_mul` | `(int, int) Result<int,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.checked_mul(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 246 | `std.math.checked_neg` | `(int) Result<int,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.checked_neg(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 247 | `std.math.checked_abs` | `(int) Result<int,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.checked_abs(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 248 | `std.math.checked_pow` | `(int, int) Result<int,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.checked_pow(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 249 | `std.math.wrapping_add` | `(int, int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.wrapping_add(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 250 | `std.math.wrapping_sub` | `(int, int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.wrapping_sub(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 251 | `std.math.wrapping_mul` | `(int, int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.wrapping_mul(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 252 | `std.math.wrapping_neg` | `(int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.wrapping_neg(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 253 | `std.math.wrapping_abs` | `(int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.wrapping_abs(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 254 | `std.math.wrapping_pow` | `(int, int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.wrapping_pow(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 255 | `std.math.saturating_add` | `(int, int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.saturating_add(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 256 | `std.math.saturating_sub` | `(int, int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.saturating_sub(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 257 | `std.math.saturating_mul` | `(int, int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.saturating_mul(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 258 | `std.math.saturating_neg` | `(int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.saturating_neg(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 259 | `std.math.saturating_abs` | `(int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.saturating_abs(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 260 | `std.math.saturating_pow` | `(int, int) int` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.saturating_pow(...)` → 精确整数/位运算返回确定值；非默认类型静态 `E3004` |
| 261 | `std.math.sinh` | `(float) Result<float,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.sinh(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 262 | `std.math.cosh` | `(float) Result<float,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.cosh(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 263 | `std.math.tanh` | `(float) Result<float,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.tanh(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 264 | `std.math.asinh` | `(float) Result<float,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.asinh(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 265 | `std.math.acosh` | `(float) Result<float,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.acosh(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 266 | `std.math.atanh` | `(float) Result<float,string>` | HUAB 22 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `math.atanh(...)` → 有限输入返回 `ok(...)`；定义域外/溢出返回 `Err` |
| 267 | `std.os.exit` | `(int) void` | HUAB 16 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `os.exit(3)` → 运行 `defer` 后进程退出码为 `3`；`os.exit(999)` 报 `E4003` |
| 268 | `std.os.platform` | `() string` | HUAB 16 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `os.platform(...)` → 返回宿主环境/平台信息 |
| 269 | `std.os.arch` | `() string` | HUAB 16 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `os.arch(...)` → 返回宿主环境/平台信息 |
| 270 | `std.os.pid` | `() int` | HUAB 16 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `os.pid(...)` → 返回宿主环境/平台信息 |
| 271 | `std.os.cpu_count` | `() int` | HUAB 16 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `os.cpu_count(...)` → 返回宿主环境/平台信息 |
| 272 | `std.os.getenv` | `(string) Result<string?,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `os.getenv("PATH")` → `ok(...)`；含 `=` 键名返回 `Err` |
| 273 | `std.os.args` | `() []string` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `os.args()` → 返回传入命令行参数切片 `[]string` |
| 274 | `std.os.cwd` | `() Result<string,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `unwrap(os.cwd())` → 返回非空当前目录路径 |
| 275 | `std.os.environ` | `() Result<map[string]string,string>` | HUAB 28 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `unwrap(os.environ())` → 返回环境变量有序 `map[string]string` |
| 276 | `std.os.parse_args` | `(argv []string, flags []string, options []string) Result<map[string][]string,string>` | HUAB 28 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `os.parse_args(["--strict","-o","out.json","in.jsonl"],["strict"],["o"])` → `ok`；未知旗标返回 `Err("ARGS_UNKNOWN...")` |
| 277 | `std.path.join` | `([]string) Result<string,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `path.join(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 278 | `std.path.normalize` | `(string) Result<string,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `path.normalize(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 279 | `std.path.basename` | `(string) Result<string,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `path.basename(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 280 | `std.path.dirname` | `(string) Result<string,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `path.dirname(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 281 | `std.path.ext` | `(string) Result<string,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `path.ext(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 282 | `std.path.relative` | `(string, string) Result<string,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `path.relative(...)` → 合法路径返回 `ok(...)`；含 NUL 或不存在路径返回 `Err` |
| 283 | `std.process.run` | `([]string, Bytes, int) (int,Bytes,Bytes,string)` | HUAB 28 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `process.run([exe, "version"], bytes.from_text(""), 5000)` → `(0, out, err, "")` |
| 284 | `std.process.run_with` | `([]string, Bytes, int, string, map<string,string>) (int,Bytes,Bytes,string)` | HUAB 28 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `process.run_with(...)` → 返回 `(exit_code, stdout, stderr, failure)`；超时 `failure=="PROCESS_TIMEOUT"` |
| 285 | `std.random.new` | `(int) std.random.Rng [writable]` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | C · 需补不透明句柄类型映射 | `random.new(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007` |
| 286 | `std.random.int` | `(mut std.random.Rng, int, int) int` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | C · 需补不透明句柄类型映射 | `random.int(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007` |
| 287 | `std.random.float` | `(mut std.random.Rng) float` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | C · 需补不透明句柄类型映射 | `random.float(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007` |
| 288 | `std.random.bool` | `(mut std.random.Rng) bool` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | C · 需补不透明句柄类型映射 | `random.bool(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007` |
| 289 | `std.random.bytes` | `(mut std.random.Rng, int) Bytes` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | C · 需补不透明句柄类型映射 | `random.bytes(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007` |
| 290 | `std.random.shuffle` | `(mut rng std.random.Rng, mut xs List<T>\|[]T) void` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `random.shuffle(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007` |
| 291 | `std.random.choice` | `(mut rng std.random.Rng, xs []T) Result<T,string>` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `random.choice(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007` |
| 292 | `std.random.sample` | `(mut rng std.random.Rng, xs []T, k int) Result<[]T,string>` | HUAB 13 | ✓ | ✗ | ✗ | ✗ | D+C · 需多态规则+类型映射 | `random.sample(rng, ...)` → 固定种子序列可复现；只读 `rng` 报 `E3003`/`E4007` |
| 293 | `std.sets.from_slice` | `(xs []K) map[K]bool [writable]` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `sets.from_slice(...)` → 返回预期容器结果 |
| 294 | `std.sets.union` | `(a map[K]bool, b map[K]bool) map[K]bool [writable]` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `sets.union(...)` → 返回预期容器结果 |
| 295 | `std.sets.intersection` | `(a map[K]bool, b map[K]bool) map[K]bool [writable]` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `sets.intersection(...)` → 返回预期容器结果 |
| 296 | `std.sets.difference` | `(a map[K]bool, b map[K]bool) map[K]bool [writable]` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `sets.difference(...)` → 返回预期容器结果 |
| 297 | `std.sets.is_subset` | `(a map[K]bool, b map[K]bool) bool` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `sets.is_subset(...)` → 返回预期容器结果 |
| 298 | `std.sets.is_disjoint` | `(a map[K]bool, b map[K]bool) bool` | HUAB 10 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `sets.is_disjoint(...)` → 返回预期容器结果 |
| 299 | `std.sets.symmetric_difference` | `(a map[K]bool, b map[K]bool) map[K]bool [writable]` | HUAB 21 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `sets.symmetric_difference(...)` → 返回预期容器结果 |
| 300 | `std.simd.backend` | `() string` | HUAB 6 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `simd.backend(...)` → 同长浮点切片返回 `ok(...)`；不等长返回 `Err` |
| 301 | `std.simd.add` | `(a []F, b []F) Result<[]F,string> [F: f32\|f64\|float]` | HUAB 6 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `simd.add(...)` → 同长浮点切片返回 `ok(...)`；不等长返回 `Err` |
| 302 | `std.simd.sub` | `(a []F, b []F) Result<[]F,string> [F: f32\|f64\|float]` | HUAB 6 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `simd.sub(...)` → 同长浮点切片返回 `ok(...)`；不等长返回 `Err` |
| 303 | `std.simd.mul` | `(a []F, b []F) Result<[]F,string> [F: f32\|f64\|float]` | HUAB 6 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `simd.mul(...)` → 同长浮点切片返回 `ok(...)`；不等长返回 `Err` |
| 304 | `std.strings.contains` | `(string, string) bool` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.contains(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 305 | `std.strings.starts_with` | `(string, string) bool` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.starts_with(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 306 | `std.strings.ends_with` | `(string, string) bool` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.ends_with(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 307 | `std.strings.index` | `(string, string) int` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.index(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 308 | `std.strings.len_bytes` | `(string) int` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.len_bytes(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 309 | `std.strings.rune_count` | `(string) Result<int,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.rune_count(...)` → 合法 UTF-8/格式返回 `ok(...)`；越界或坏格式返回 `Err` |
| 310 | `std.strings.trim` | `(string) string` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.trim("  hi\n")` → `"hi"` |
| 311 | `std.strings.lower_ascii` | `(string) string` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.lower_ascii(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 312 | `std.strings.upper_ascii` | `(string) string` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.upper_ascii(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 313 | `std.strings.split` | `(string, string) []string` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.split(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 314 | `std.strings.join` | `([]string, string) string` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.join(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 315 | `std.strings.replace` | `(string, string, string) string` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.replace(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 316 | `std.strings.slice` | `(string, int, int) Result<string,string>` | HUAB 3 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.slice(...)` → 合法 UTF-8/格式返回 `ok(...)`；越界或坏格式返回 `Err` |
| 317 | `std.strings.last_index` | `(string, string) int` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.last_index(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 318 | `std.strings.count` | `(string, string) int` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.count(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 319 | `std.strings.repeat` | `(string, int) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.repeat(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 320 | `std.strings.split_n` | `(string, string, int) []string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.split_n(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 321 | `std.strings.fields` | `(string) []string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.fields(" a  b\tc ")` → `["a","b","c"]` |
| 322 | `std.strings.lines` | `(string) []string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.lines("a\r\nb\n")` → `["a","b"]` |
| 323 | `std.strings.trim_start` | `(string) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.trim_start(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 324 | `std.strings.trim_end` | `(string) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.trim_end(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 325 | `std.strings.trim_prefix` | `(string, string) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.trim_prefix(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 326 | `std.strings.trim_suffix` | `(string, string) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.trim_suffix(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 327 | `std.strings.pad_left` | `(string, int, string) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.pad_left(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 328 | `std.strings.pad_right` | `(string, int, string) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.pad_right(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 329 | `std.strings.replace_n` | `(string, string, string, int) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.replace_n(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 330 | `std.strings.is_empty` | `(string) bool` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.is_empty(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 331 | `std.strings.parse_int` | `(string, int) Result<int,string>` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.parse_int("42", 10)` → `ok(42)`；`"42x"` 返回 `Err` |
| 332 | `std.strings.parse_float` | `(string) Result<float,string>` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.parse_float("3.5")` → `ok(3.5)`；`"nan"` 返回 `Err` |
| 333 | `std.strings.format_int` | `(int, int) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.format_int(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 334 | `std.strings.format_float` | `(float, int) string` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.format_float(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 335 | `std.strings.format` | `(string, []string) Result<string,string>` | HUAB 9 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.format("{}:{}", ["line", "1"])` → `ok("line:1")`；占位符不匹配返回 `Err` |
| 336 | `std.strings.center` | `(string, int, string) string` | HUAB 23 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.center(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 337 | `std.strings.codepoints` | `(string) []int` | HUAB 23 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.codepoints(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 338 | `std.strings.chars` | `(string) []string` | HUAB 23 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.chars(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 339 | `std.strings.from_codepoints` | `([]int) Result<string,string>` | HUAB 23 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.from_codepoints(...)` → 合法 UTF-8/格式返回 `ok(...)`；越界或坏格式返回 `Err` |
| 340 | `std.strings.slice_runes` | `(string, int, int) Result<string,string>` | HUAB 23 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.slice_runes(...)` → 合法 UTF-8/格式返回 `ok(...)`；越界或坏格式返回 `Err` |
| 341 | `std.strings.template` | `(string, map<string,string>) Result<string,string>` | HUAB 25 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.template("{k:>4}", map[string]string{"k":"7"})` → `ok("   7")`；缺键返回 `Err` |
| 342 | `std.strings.format_int_grouped` | `(int, string) string` | HUAB 25 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.format_int_grouped(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 343 | `std.strings.format_float_grouped` | `(float, int, string) string` | HUAB 25 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.format_float_grouped(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 344 | `std.strings.upper` | `(string) string` | HUAB 26 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.upper(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 345 | `std.strings.lower` | `(string) string` | HUAB 26 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.lower(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 346 | `std.strings.casefold` | `(string) string` | HUAB 26 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.casefold(...)` → ASCII/UTF-8 输入返回预期结果；超 16 MiB 报 `E4099` |
| 347 | `std.strings.equal_fold` | `(string, string) bool` | HUAB 26 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `strings.equal_fold("Straße", "STRASSE")` → `true` |
| 348 | `std.task.sleep` | `(int) void` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.sleep(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 349 | `std.task.yield` | `() void` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.yield(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 350 | `std.task.worker_id` | `() string` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.worker_id(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 351 | `std.task.cancel` | `(t Task<T>) bool` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.cancel(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 352 | `std.task.done` | `(t Task<T>) bool` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.done(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 353 | `std.task.state` | `(t Task<T>) string` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.state(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 354 | `std.task.timeout` | `(t Task<T>, ms int) Result<T,string>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.timeout(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 355 | `std.task.all` | `(a Task<T>, b Task<T>) Task<[]T>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.all(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 356 | `std.task.race` | `(a Task<T>, b Task<T>) Task<T>` | HUAB 6 | ✓ | ✓ | ✓ | ✗ | C · 需 Task 原生/自举特化 | `task.race(...)` → 遵循协作取消 `E4101` 与结构化作用域规则 |
| 357 | `std.test.assert` | `(bool, string) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `test.assert(...)` → 条件成立无输出；失败抛 `E4900` |
| 358 | `std.test.equal` | `(actual T, expected T) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `test.equal(...)` → 条件成立无输出；失败抛 `E4900` |
| 359 | `std.test.not_equal` | `(actual T, unexpected T) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | D · 需多态签名规则 | `test.not_equal(...)` → 条件成立无输出；失败抛 `E4900` |
| 360 | `std.test.fail` | `(string) void` | HUAB 18 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `test.fail(...)` → 条件成立无输出；失败抛 `E4900` |
| 361 | `std.time.since_ns` | `(int) int` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.since_ns(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 362 | `std.time.format_duration` | `(int) string` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.format_duration(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 363 | `std.time.parse_duration` | `(string) Result<int,string>` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.parse_duration(...)` → 合法时间文本/分量返回 `ok(int)`；非法格式返回 `Err` |
| 364 | `std.time.utc_parts` | `(int) (int,int,int,int,int,int,int)` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.utc_parts(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 365 | `std.time.utc_from_parts` | `(int, int, int, int, int, int, int) Result<int,string>` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.utc_from_parts(...)` → 合法时间文本/分量返回 `ok(int)`；非法格式返回 `Err` |
| 366 | `std.time.weekday` | `(int) int` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.weekday(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 367 | `std.time.day_of_year` | `(int) int` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.day_of_year(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 368 | `std.time.is_leap_year` | `(int) bool` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.is_leap_year(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 369 | `std.time.days_in_month` | `(int, int) int` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.days_in_month(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 370 | `std.time.format_rfc3339` | `(int) string` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.format_rfc3339(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 371 | `std.time.format_rfc3339_offset` | `(int, int) string` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.format_rfc3339_offset(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 372 | `std.time.parse_rfc3339` | `(string) Result<int,string>` | HUAB 15 | ✓ | ✗ | ✗ | ✗ | B · 仅差纯表项/桥接登记 | `time.parse_rfc3339(...)` → 合法时间文本/分量返回 `ok(int)`；非法格式返回 `Err` |
| 373 | `std.time.monotonic_ns` | `() int` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `time.monotonic_ns(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 374 | `std.time.unix_ms` | `() int` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通 | `time.unix_ms(...)` → 纪元毫秒/单调时钟计算确定结果 |
| 375 | `std.alg_integer.lower_bound` | `(values []int, key int) int` | HUAB 6 | ✓ | ✓ | ✓ | ✓ | A · 已全路径打通（SDK 源码） | `alg_integer.lower_bound([1,4,4,7], 4)` → `1`；未设 `HUA_HOME` 报 `E5001` |

## 4. 工具链级特性补充说明

- **`@test` 与 `hua test <file.hua> [--filter text]`**：属于 CLI 驱动行为（`src/main.cpp:43`），当前调用 C++ `ModuleLoader` + `SemanticAnalyzer` + `VirtualMachine` 按源码顺序独立执行顶层 `@test fn`。Step 1 仓库外多包完成后，只要 C++ `ModuleLoader` 接入相同的本地包解析规则，`hua test` 即可直接测试多包项目与独立包，无需等 `native_cpp.hua` 单独支持 `@test`。

