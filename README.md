# Hua 0.1.0-dev

从设计思想开始系统学习、检查每项规则，请先读 [Hua语言百科与设计核对](docs/语言百科与设计核对.md)：36章、207个编号条目，已有内容填入，未定结论留空，可逐项确认。


轻量系统脚本语言。当前完成 **前端、基础语义、Map/多返回/Result/Optional 常用语义、栈式字节码 VM、`.huab` 文件读写、本地源码/Native/WASM 模块加载，以及程序输入/文本与二进制文件/字符串/JSON/动态列表/缓冲标准库，以及可选嵌入式 Python Bridge；本轮加入 interface/enum/match、泛型、闭包/defer、精确数值与固定数组布局**。

写好第一个程序后：保存为main.hua，在它所在目录的VS Code终端输入 `hua run .\main.hua`。详细步骤和找不到hua时的处理见 [第一个程序怎么运行](docs/开发与运行指南.md)。

先读 [项目概览与进度](docs/项目概览与进度.md)：已完成什么、尚缺什么、现在能做什么，以及入门/构建/部署步骤。文档按改动涉及的内容维护，无需全量同步。
全部文档的中文文件名和历史资料入口见 [文档目录](docs/文档目录.md)。运行耗时测量、应用模块布局和编译器目录说明见 [运行计时与模块结构](docs/开发与运行指南.md)。
第一批25接口（数学/算法/时钟/getenv）已实现，见 [数学与算法接口合同](docs/标准库与内建接口.md)，可运行examples/algorithms_config.hua。
标准库补齐计划与算法/Agent能力差距见 [标准库设计与实现路线](docs/标准库设计与实现路线.md)。
标准库和公开函数清单：[标准库与内建接口](docs/标准库与内建接口.md)。

```powershell
.\scripts\build.ps1
.\build\hua.exe version
.\build\hua.exe check .\examples\hello.hua
.\build\hua.exe ast .\examples\hello.hua
.\build\hua.exe run .\examples\hello.hua
.\build\hua.exe run .\examples\fibonacci.hua
.\build\hua.exe run .\examples\struct.hua
.\build\hua.exe run .\examples\binary_search.hua
.\build\hua.exe run .\examples\modules\main.hua
.\build\hua.exe run .\examples\comments.hua
.\build\hua.exe bytecode .\examples\modules\main.hua
.\build\hua.exe interpret .\examples\modules\main.hua
```

`run hello.hua` 输出 `Hua`；`run struct.hua` 输出 `5`；fibonacci 输出前十个斐波那契数。
`check` 加载完整本地依赖图，再检查作用域、声明、可变性、函数签名和已知基础类型。
`ast` 只解析当前文件；`run` 在全部依赖通过语义检查后编译为指令并交给 VM 执行。
`interpret` 保留原解释器供对照；`bytecode` 只显示指令，不执行。
退出码：0 成功，1 源码/语义/执行/读取错误，2 命令用法错误。

## 构建

C++20 + CMake 3.20+ + Wasmtime C API SDK。Wasmtime SDK 与编译工具包不提交到 Git。克隆后请从 [官方 Release](https://github.com/bytecodealliance/wasmtime/releases/tag/v49.0.2) 获取对应平台 C API SDK，解压到 `.tools/` 的默认目录，或用 `-DWASMTIME_ROOT=...` 指定。

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

本机的 `scripts/build.ps1` 复用既有 Android SDK CMake/Ninja 和 `.tools/` 便携式 LLVM-MinGW。
脚本不下载或安装工具。Windows CLI 静态链接 C++ 库，支持中文文件路径。
运行目录须保留 `hua.exe` 与 `wasmtime.dll`；Native 模块还需要其动态库。SDK 来源、许可证与校验值见 Phase 4 文档。
本次实际验证的是 Windows Clang 23.1.2；未宣称跨平台工具链全部实测。

## 可选 Python 包兼容

独立 Native 插件可嵌入 CPython 3.12，导入现成 Python 包并调用函数、类/方法；复杂对象使用句柄，基础数据通过 Json 交换。普通 Hua 核心不链接 Python，插件默认不构建。

```powershell
.\scripts\build.ps1 -PythonBridge -PythonHome 'C:\Users\liqia\AppData\Local\Programs\Python\Python312'
.\build\hua.exe run .\build\python_bridge\main.hua
```

输出 `Python sqrt: 9`、`missing package: true`、`handles: 0`。
本机还验证 packaging 26.3 的类实例与属性，以及解释器/VM/无源码 HUAB 对照；其他包需按 CPython 版本/平台/外部依赖单独验收。
完整 13 个接口、pip 项目安装、部署/释放/错误与兼容边界见 [Python Bridge合同](docs/运行时与扩展设计.md)。这是调用 Python 库，未提供自动转为 Hua 库的工具。

## 语义与执行

- 词法作用域、同层重复声明、未定义名称和声明顺序检查。
- `let`/`const` 不可重绑，`var` 可重绑；只读 struct/视图不可修改。
- const 支持编译期标量、常量引用和基本运算；不允许运行时变量、容器和函数调用作为常量求值输入。
- 函数、递归、函数值、参数/返回检查；无类型注解的信息不足时由运行时补充检查。
- bool 条件、短路逻辑、if/else、while、for、range/by、break/continue、return。
- struct 值复制、具名字段、普通方法；分析方法直接/间接 self 写入，要求可变 receiver。
- Slice 零复制 view；只读能力不能通过 var 别名升级；clone 深复制后可独立修改。
- Slice 参数默认只读；`mut []T` 允许修改传入的可变视图。
- 基础 int/float/bool/string/nil、算术/比较/位运算和显式转换。
- Map 构造/索引/缺失 nil/has/delete/快照遍历；多返回、逐项绑定、交换与 `_` 丢弃。
- Result<T,E>、ok/err、解包和 `?` 传播；Optional 检测/解包与不可重绑局部名称的 nil 分支收窄。
- core：print、str、int、float、len、clone、sqrt、min/max、abs、clamp、type，以及 has/delete、ok/err、is_ok/is_err、unwrap/unwrap_err/unwrap_or、is_some/is_none。

新能力的完整规则与运行例子见 [值与结果规则](docs/语言设计与类型规则.md) 和 [values.hua](examples/values.hua)。

执行顺序：登记函数/struct，按依赖顺序初始化模块一次，执行入口顶层语句，自动调用入口零参数 main（若存在）。
函数使用模块词法环境，不读取调用者的局部变量。所有源码先检查，再开始产生执行副作用。

## 程序输入与基础标准库

已提供 std.os、std.io、std.fs、std.strings、std.json、std.bytes、std.buffer、std.list，共 73 个函数。文件/输入/JSON 的可恢复错误返回 Result。

```powershell
'hello' | .\build\hua.exe run .\examples\standard_library.hua -- .\examples\data\profile.json
.\build\hua.exe build .\examples\standard_library.hua -o .\build\standard_library.huab
'hello' | .\build\hua.exe run .\build\standard_library.huab -- .\examples\data\profile.json
```

输出 `HUA:3:hello`。参数在 `--` 后，文件相对路径基于工作目录，导入相对入口目录；check/build 不执行 I/O。
完整接口、JSON 数值/UTF-8/资源边界及解释器对照命令见 [基础标准接口](docs/标准库与内建接口.md)。
网络与数据库按用户选择留到后续阶段；当前没有 HTTP/SQLite 或文件流句柄接口。

## 字节、动态列表与缓冲

Bytes 保存任意字节，Buffer 支持追加文本/小端整数，List<T> 支持 append/extend/pop 与快照。修改需 var 或 mut 参数，clone 获得独立内容。

```powershell
.\build\hua.exe run .\examples\binary_collections.hua -- .\build\bootstrap.bin
.\build\hua.exe build .\examples\binary_collections.hua -o .\build\binary_collections.huab
.\build\hua.exe run .\build\binary_collections.huab -- .\build\bootstrap.bin
```

示例写入 17 字节并读回验证，输出与解释器对照见 [字节与容器合同](docs/标准库与内建接口.md)。
本阶段补齐自举基础设施，Hua 版本编译器与自举闭环仍未实现。

## 本地模块

```hua
import geometry as geo
var p = geo.Point{x: 3.0, y: 4.0}
print(p.length())
```

上例使用 `examples/modules/geometry/package.hua` 的公开 Point 与方法。
入口目录为解析根，`import a.b` 先寻找 `a/b.hua`，再找 `a/b/package.hua`。
无别名时以 `a.b.name` 访问；支持 `pub fn`、`pub struct`、限定类型和具名初始化。
导入位于文件顶部；私有接口、缺失模块、循环与命名冲突均有定位诊断。
重复导入和菱形依赖共享缓存，仅初始化一次；依赖模块的 main 不自动调用。
模块测试和基础规则见 [VM/模块规则](docs/运行时与扩展设计.md)。

## HUAB 与扩展

```powershell
.\build\hua.exe build .\examples\modules\main.hua -o .\build\modules.huab
.\build\hua.exe check .\build\modules.huab
.\build\hua.exe run .\build\modules.huab
.\build\hua.exe run .\build\extensions\main.hua
.\build\hua.exe build .\build\extensions\main.hua
.\build\hua.exe run .\build\extensions\main.huab
```

`.huab` 保存整个程序的指令、声明与诊断源码；WASM 字节也嵌入文件。运行时无需原源码。
`build` 默认输出同名 `.huab`；`check`/`bytecode` 接受 `.huab`，读取时检查格式、CRC 与控制流栈状态。
源码模块之后依次查找 `.huam` / `package.huam`（Native 接口清单）、`.wasm` / `package.wasm`。
Native 使用 `include/hua/native.h` 的 opaque C ABI v1，支持标量与字符串；WASM 支持 i32/i64/f32/f64 导出函数，无 host imports/WASI。
Native DLL 不嵌入文件，需按原相对路径放在 HUAB 目录内；Native 是可信的同进程代码。
扩展示例输出 `42 42`、`hello Hua true`、`5 7`。
格式、接口、资源上限和部署方法见 [HUAB / Native / WASM](docs/运行时与扩展设计.md)。

## 已冻结的运算符与注释规则

`/` 为普通真除法，`//` 为整数整除，`%` 为取余，`**` 为幂。

```hua
#!/usr/bin/env hua
let a = 7 / 2 # 3.5
let b = 7 // 2 # 3
let c = 7 % 2 # 1
#* outer #* inner *# outer *#
```

`//` 与 `* / %` 同优先级、左结合，支持 `//=`。当前整除仅接受 int，负数向下取整。
负数取模满足 `a = (a // b) * b + a % b`；余数与除数同符号或为 0。

单行注释为 `# comment`；块注释为 `#* ... *#`，支持嵌套。
`##` 与 `#** ... **#` 分别预留为文档单行与文档块注释；当前按注释跳过，不生成文档或 AST 节点。
文件第一行的 `#!...` 是 shebang，由 Lexer 忽略；其他位置的 `#!` 按普通 `#` 行注释处理。
`//` 在所有位置只表示整除，不再识别为注释；`/* ... */` 不再是 Hua 注释。
Lexer 用 depth 计数处理嵌套，并记录各层普通/文档闭合标记；注释换行保留 NEWLINE。
字符串中的所有注释标记保持字面内容。完整规则见 `docs/语言设计与类型规则.md`。

## 当前限制

当前 VM 覆盖基础语言子集，不是完整 Hua V0.1：

- int/float 仍为有界 int64/有限 float64；精确 i/u/f 尺寸类型、显式转换与溢出检查已实现。
- 普通二元运算不隐式混合 int/float；浮点上下文可接收可精确表示的整数**字面量**。
- `float ** int` 支持整数指数，满足规范 Point.length 示例；整数负指数暂拒绝。
- 固定数值/bool/嵌套数组使用连续小端布局；非平凡元素仍是受管理 Value 槽位，不等于 C ABI 布局。
- 已有局部 Optional 分支/循环/早退收窄和已知调用的返回写能力推断；动态函数、深层字段别名仍保守。Optional<Result>、Map 直接 Result 值、通用 Tuple 尚未实现。
- ref/ptr 执行、unsafe 内存操作及复杂 FFI 仍未实现。
- 本地源码/Native/WASM 与 std.os/io/fs/strings/json/bytes/buffer/list 已加载；包管理、选择导入及其他标准模块尚未实现。
- `.huab` 是完整可执行程序，不能作为 import 模块；尚无自动磁盘编译缓存；局部函数与结构已执行，闭包捕获只读快照。
- GC/async/parallel/SIMD 当前合同见 Phase 10；事件循环、通用自动 SIMD、Web/AI 库、LLVM 后端和 AOT 尚未完成。
- VM 默认最多 1,000,000 条指令、128 层函数调用；超限 E4099。
- 本地模块图最多 128 文件/64 依赖层/64 MiB 源码，单文件 16 MiB。
- check 不是完整静态类型证明；动态值、索引、别名和数值边界仍需运行时检查。

## S1a：本地路径与文件状态

新增`std.path.join/normalize/basename/dirname/ext/relative`和`std.fs.stat/rename/replace/temp_file`；当前15模块/125函数。路径为宿主纯词法处理；rename不覆盖，replace同卷普通文件改名，temp排他创建并关闭，由调用者删除。

```powershell
.\build\hua.exe check .\examples\local_paths.hua
.\build\hua.exe run .\examples\local_paths.hua
.\build\hua.exe interpret .\examples\local_paths.hua
.\build\hua.exe build .\examples\local_paths.hua -o .\build\local_paths.huab
.\build\hua.exe run .\build\local_paths.huab
```

输出`b demo.txt .txt`和`file 8`。错误、资源和平台边界见 [S1a合同](docs/标准库与内建接口.md#s1a路径与文件状态详细合同)。本次Windows验收；POSIX/第二卷未实测，不承诺断电持久提交。下一切片S1b JSON/配置。

## 文件与验证

`include/hua` 和 `src` 保持前端、语义、值运算、解释器、编译器、VM 和模块加载分层。
2026-10-08 开启可选 Python Bridge 的完整CTest22/22通过（含GC/任务/并发、自举词法器与插件测试，桥接可选）。`tests/spec`：100 个语法样例、2608 检查；`tests/runtime`：187 个样例各跑两种引擎。
另有 114 项 VM/模块 CLI、46 项原 CLI、551 项 HUAB/Native/WASM CLI 检查，126 个持久化运行样例及 30 次 CRC 修复随机变异检查；stdout 与错误均有独立预期。
另有 269 项输入/标准库/JSON、185 项字节/列表/缓冲 CLI 检查，对照解释器/VM/无源码 HUAB，验证实际容量边界与独立 Python 字节/CRC。
HUAB 写入 v6、读取 v1–v6；Native ABI 仍为 1。
桥接新增 53 项离线对照，提供真实 packaging 26.3 路径时共 61 项；独立关闭桥接构建能运行 quickstart，核心无 Python 链接依赖。

原始 Nova 规范保留，Hua 命名副本及当前决定见：

- [项目概览与进度](docs/项目概览与进度.md)
- [开发与运行指南](docs/开发与运行指南.md)
- [语言设计与类型规则](docs/语言设计与类型规则.md)
- [运行时与扩展设计](docs/运行时与扩展设计.md)
- [标准库与内建接口](docs/标准库与内建接口.md)
- [标准库设计与实现路线](docs/标准库设计与实现路线.md)
- [设计决策与未决事项](docs/设计决策与未决事项.md)

## 仓库与许可证

项目仓库：[NssSSLY/Hua](https://github.com/NssSSLY/Hua)。沿用仓库已有的 [Apache 2.0 许可证](LICENSE)。
`.tools/`、`build*/`、`.hua/` 缓存和生成的 `.huab` 不提交，源码 `.huam` 接口清单正常保存。

## Phase 9 语言与类型示例

接口符合性、带数据枚举与穷尽 match、显式用户泛型、局部函数/结构、词法块 defer 已可在两种执行器与 HUAB 中使用。
精确数值和固定数组支持范围/溢出检查及 sizeof/alignof；已知调用图推断容器返回写能力。

```powershell
.\build\hua.exe run .\examples\language_features.hua
.\build\hua.exe build .\examples\language_features.hua -o .\build\language_features.huab
.\build\hua.exe run .\build\language_features.huab
```

97 个新增用例、549 项检查通过。语法、复制/捕获/布局规则和未开放项见 [语言与类型规则](docs/语言设计与类型规则.md)。

## GC、异步任务与并行计算

专用地址稳定GC已接入。async/await/spawn、Task<T>、结构化taskgroup，以及std.task取消/毫秒超时/all/race可运行。
parallel for支持独立数组计算，最多8线程块；simd for为受检查提示，std.simd.add/sub/mul使用实际SSE2浮点向量运算，无SSE2时采用标量后备。
标准库125接口、HUAB writer6/reader1–6；Native ABI1与可选Python接口不变。任务采用私有深复制快照，禁止跨任务可变借用/全局写入、stdin和后台Native/WASM/Python调用。事件循环、async方法、Channel、通用自动SIMD仍待完成。

```powershell
.\build\hua.exe run .\examples\concurrency.hua
.\build\hua.exe interpret .\examples\concurrency.hua
.\build\hua.exe build .\examples\concurrency.hua -o .\build\concurrency.huab
.\build\hua.exe run .\build\concurrency.huab
```

完整规则和输出见 [GC与并发合同](docs/运行时与扩展设计.md)。新增68用例、409项检查和当时完整CTest16/16通过（当前最新结果见项目概览）；GC根/循环、真实线程屏障/64上限、取消清理、SSE2指令和独立关闭Python构建均已核对。


## 本地安装、VS Code 与自举起步

现在可把核心安装到用户目录并配置PATH；默认使用build-python-off的hua.exe和wasmtime.dll。不需要为运行普通Hua程序安装C++工具链或Python。

```powershell
.\scripts\install-local.ps1 -AddToUserPath
hua run .\examples\hello.hua
hua build .\examples\hello.hua -o .\build\hello.huab
hua run .\build\hello.huab
code --install-extension .\build\hua-language-0.1.0.vsix
```

安装包不存在时运行 `python .\scripts\package-vscode.py`。首次构建请先用build.ps1（可指定-BuildDirectory build-python-off）或前文CMake方式生成核心；其他构建可传安装脚本-BuildDirectory。配置用户PATH后重新打开其他终端/VS Code。
基础插件源码在 [editors/vscode](editors/vscode/README.md)：高亮、片段、保存检查和检查/运行/构建命令；无LSP、定义跳转或调试器。
Hua编写的 [词法器](selfhost/lexer.hua) 已能扫描自身源码；135输入、541项进程检查对照C++/解释器/VM/移除源码后的HUAB。插件6项针对性测试及真实VS Code隔离安装通过。

```powershell
hua run .\selfhost\main.hua -- .\selfhost\lexer.hua
```

完整自举仍缺Hua Parser/AST、语义与降级、字节码/HUAB生成、连续编译自身比较。安装、限制与分阶段路线见 [开发与自举指南](docs/开发与运行指南.md)。

错误基础新增Result<void,E>与只读std.error.Value，默认string错误保持。当前16标准模块/141函数。运行`.\build\hua.exe run .\examples\error_foundation.hua`；[Error接口与逐接口盘点](docs/标准库与内建接口.md#standard-error-contract)、[本轮验收与边界](docs/项目概览与进度.md#er-foundation-verification)。普通HUAB写6，新错误能力写7，读1–7；Native ABI1保持。
