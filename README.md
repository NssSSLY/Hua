# Hua 0.1.0-dev

轻量系统脚本语言。当前完成 **前端、基础语义、栈式字节码 VM、`.huab` 文件读写与本地源码/Native/WASM 模块加载**。

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

## 语义与执行

- 词法作用域、同层重复声明、未定义名称和声明顺序检查。
- `let`/`const` 不可重绑，`var` 可重绑；只读 struct/视图不可修改。
- const 支持编译期标量、常量引用和基本运算；不允许运行时变量、容器和函数调用作为常量求值输入。
- 函数、递归、函数值、参数/返回检查；无类型注解的信息不足时由运行时补充检查。
- bool 条件、短路逻辑、if/else、while、for、range/by、break/continue、return。
- struct 值复制、具名字段、普通方法；分析方法直接/间接 self 写入，要求可变 receiver。
- Slice 零复制 view；只读能力不能通过 var 别名升级；clone 深复制后可独立修改。
- Slice 参数默认只读；`mut []T` 允许修改传入的可变视图。
- 基础 int/float/bool/string/nil、Optional 的 nil 返回、算术/比较/位运算和显式转换。
- core：print、str、int、float、len、clone、sqrt、min/max、abs、clamp、type。

执行顺序：登记函数/struct，按依赖顺序初始化模块一次，执行入口顶层语句，自动调用入口零参数 main（若存在）。
函数使用模块词法环境，不读取调用者的局部变量。所有源码先检查，再开始产生执行副作用。

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
模块测试和基础规则见 [VM/模块规则](docs/PHASE3_VM_MODULES.md)。

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
格式、接口、资源上限和部署方法见 [HUAB / Native / WASM](docs/PHASE4_ARTIFACTS_EXTENSIONS.md)。

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
字符串中的所有注释标记保持字面内容。完整规则见 `docs/LEXER_PARSER.md`。

## 当前限制

当前 VM 覆盖基础语言子集，不是完整 Hua V0.1：

- int 暂用有溢出检查的 int64，float 暂用有限 float64；这不代表全部数值规则永久冻结。
- 普通二元运算不隐式混合 int/float；浮点上下文可接收可精确表示的整数**字面量**。
- `float ** int` 支持整数指数，满足规范 Point.length 示例；整数负指数暂拒绝。
- 支持固定数组的类型/长度检查，但暂用 Slice 存储，不承诺底层固定布局。
- 多返回绑定/执行、Result 传播、Map 执行、精确宽度类型、ref/ptr 执行和 unsafe/FFI 仍未实现。
- 本地源码/Native/WASM 已加载；标准模块命名空间、包管理与选择导入尚未实现。
- `.huab` 是完整可执行程序，不能作为 import 模块；尚无自动磁盘编译缓存；嵌套函数/struct 仍不执行。
- 尚无 Hua GC、async、parallel/SIMD、Web/AI 库、LLVM 后端或 AOT。
- VM 默认最多 1,000,000 条指令、128 层函数调用；超限 E4099。
- 本地模块图最多 128 文件/64 依赖层/64 MiB 源码，单文件 16 MiB。
- check 不是完整静态类型证明；动态值、索引、别名和数值边界仍需运行时检查。

## 文件与验证

`include/hua` 和 `src` 保持前端、语义、值运算、解释器、编译器、VM 和模块加载分层。
CTest 9/9 通过。`tests/spec`：99 个语法样例、2405 检查；`tests/runtime`：119 个样例各跑两种引擎。
另有 114 项 VM/模块 CLI、38 项原 CLI、368 项 HUAB/Native/WASM CLI 检查，80 个持久化运行样例及 30 次 CRC 修复随机变异检查；stdout 与错误均有独立预期。
原始 Nova 规范保留，Hua 命名副本及当前决定见：

- [设计决定](docs/HUA_DECISIONS.md)
- [Lexer/Parser 规则](docs/LEXER_PARSER.md)
- [Phase 2 语义与执行规则](docs/PHASE2_SEMANTICS.md)
- [Phase 3 VM/模块规则](docs/PHASE3_VM_MODULES.md)
- [Phase 4 HUAB/扩展规则](docs/PHASE4_ARTIFACTS_EXTENSIONS.md)
- [未决问题](docs/OPEN_QUESTIONS.md)
- [项目交接与测试结果](docs/CODEX_HUA_HANDOFF.md)

## 仓库与许可证

项目仓库：[NssSSLY/Hua](https://github.com/NssSSLY/Hua)。沿用仓库已有的 [Apache 2.0 许可证](LICENSE)。
`.tools/`、`build*/`、`.hua/` 缓存和生成的 `.huab` 不提交，源码 `.huam` 接口清单正常保存。
