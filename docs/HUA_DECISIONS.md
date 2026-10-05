# Hua 当前决定与分阶段实现基线

日期：2026-10-05。状态：Phase 1 完成，Phase 2 基础语义、Phase 3 VM/本地模块与 Phase 4 HUAB/Native/WASM 里程碑完成；语言仍为 0.1 草案。

## 来源

1. 本地 `Nova_Language_Specification_v0.1.md`（历史输入，完整保留）。
2. 用户引用的 ChatGPT「创建轻量编程语言」，会话 ID `6ac3364e-4970-83ea-874c-5ead85119106`。
3. 来自协调会话的原对话关键决定摘要：最终名称改为 Hua，并确认本阶段仅实现前端。

本项目保存的是规范及获取到的决定摘要，**不是完整原聊天的归档**。

## 命名

| 项目 | 最终名称 |
|---|---|
| 语言 / CLI | Hua / hua |
| 源文件 | .hua |
| 包配置 / 锁文件 / 缓存 | hua.toml / hua.lock / .hua/ |
| 字节码 / 模块 | .huab / .huam |
| 环境变量 | HUA_HOME / HUA_PATH / HUA_CACHE |

包管理配置和环境变量仍仅登记。当前实现 `.huab` 完整程序格式 v1，`.huam` 是 Native ABI v1 接口清单。
已实现内存 VM、本地源码/Native/WASM 加载，见 PHASE3_VM_MODULES.md 和 PHASE4_ARTIFACTS_EXTENSIONS.md。
规范副本中的概念 C API 不等于实际接口；当前可用头文件为 include/hua/native.h。

## 稳定原则

- struct + method + interface + composition，不做 class/继承。
- Value → Slice []T → safe ref<T> → unsafe ptr<T>。
- 不引入 Rust lifetime / borrow checker。
- 小核心、分层前端、可用错误信息优先；普通代码不获得裸指针能力。
- Parser 不执行代码，所有节点保留 SourceSpan；Native ABI 与未来 VM 私有结构隔离。

## Phase 1 的历史范围

C++20 + CMake；Token、Lexer、Parser、AST、AstPrinter、Diagnostic。
CLI：`hua version`、`hua check <file>`、`hua ast <file>`。
支持规范中的变量、函数/方法、struct、控制流、基础表达式、数组/切片、类型和 unsafe 的解析。
Phase 1 的 check 仅语法检查。Phase 2 已扩展为基础语义检查，仍不是完整静态类型/引用证明。

不提前实现 Interpreter/VM/GC/FFI/Async Runtime/LLVM 后端/AOT/SIMD 执行。
使用 Clang 编译 C++ 项目不意味着实现了 LLVM 语言后端。
不初始化 Git，不推送，不修改其他项目。

## 暂定的最小语法取舍

- 根据正式优先级章节：幂高于一元运算，且幂右结合。`-2**2` 解析为 `-(2**2)`；
  `2**3**2` 解析为 `2**(3**2)`。EBNF 的 unary/power 冲突见 OPEN_QUESTIONS。
- 保留 NEWLINE；完整表达式后的换行结束语句，`()`/`[]` 和具名初始化列表内忽略换行，
  二元/一元运算符后的换行可继续未完成表达式。块和声明的 `{` 跟随头部。
- `/` 真除法、`//` 整除、`%` 取余与 `**` 幂保留；注释标记已冻结，见下节。
- Range 只在 for 头部构造，`by`/`mut` 为上下文词，不增加普通保留字。
- struct 字面量以 `{ IDENT : ... }` 区分条件块；空 struct 字面量在条件头部须用括号。
- 不实现未冻结的参数可变性执行规则；保留 `mut` 类型/参数信息。
- 声明须带初始化；不添加尾逗号、分号或位置 struct 初始化语法。
- Source 文本接受 UTF-8 BOM、LF/CRLF/CR，内部去 BOM 并规范为 LF。
  SourceSpan 的偏移/列是规范化 UTF-8 文本的字节位置，行从 1 开始。
- 深层或超长表达式报告 E2008；CLI 源码读取上限 16 MiB，防止前端栈/内存失控。

以上是有限实现边界，不是宣称所有语言问题已冻结。待决定的事项集中于 OPEN_QUESTIONS。

## Phase 2 当前决定

用户明确授权开始语义分析与解释器，已增加 SemanticAnalyzer/SemanticModel、Value、Environment、Interpreter 和 hua run。
当前可运行函数、递归、struct/方法、Slice、控制流及基础 core；详细暂定语义见 PHASE2_SEMANTICS.md。
check 仅检查；run 通过检查后执行；ast 保留仅解析。未实现功能不会偷偷映射为宿主语言行为。

## 2026-10-05 冻结更新：运算符与注释

本次用户直接指令取代此前“注释暂缓”与行首/行内 `//` 临时区分规则。
已冻结：`//` 为整数整除（`7 // 3 == 2`），`/` 普通除法、`%` 取余、`**` 幂。
已有 `//=` 保留；`//` 的识别不依赖所在行或前面是否有代码。

单行注释为 `# comment`；块注释为 `#* ... *#`，支持嵌套。
`##` 与 `#** ... **#` 分别预留为文档单行与文档块注释；当前按注释跳过，不生成文档或 AST 节点。
文件第一行的 `#!...` 是 shebang，由 Lexer 忽略；其他位置的 `#!` 按普通 `#` 行注释处理。
`//` 在所有位置只表示整除，不再识别为注释；`/* ... */` 不再是 Hua 注释。
Lexer 用 depth 计数处理嵌套，并记录各层普通/文档闭合标记；注释换行保留 NEWLINE。
字符串中的所有注释标记保持字面内容。完整规则见 `docs/LEXER_PARSER.md`。

int64/有限 float64、readonly Slice、方法自动副作用标记、资源上限都是当前开发实现边界，
不是所有 V0.1 未决问题已永久冻结。程序示例成功执行与剩余限制均有独立测试和文档。

## 2026-10-05 Phase 3：VM 与本地模块

用户授权完成 VM、模块加载并核查注释文档。run 已切换为 BytecodeCompiler → VirtualMachine；
interpret 保留 Tree-Walk 对照，bytecode 显示来源位置与指令。VM 不回退调用 Interpreter。
ModuleLoader 统一解析 `<入口根>/a/b.hua` 与 `a/b/package.hua`，源码文件优先。
实现 pub fn/struct/方法、别名、限定类型、模块隔离、canonical 路径缓存、循环拒绝和依赖初始化一次。
整个图先解析/链接/语义检查，再运行；入口 main 仅在依赖完成后自动调用。
Phase 3 当时尚无磁盘字节码与外部模块；Phase 4 已增加 HUAB v1 与 Native/WASM。包管理、标准模块命名空间和 HUA_PATH 搜索仍未实现。
注释标记保持冻结方案，补充可运行例子、换行与闭合规则；不恢复旧 Go 注释临时方案。
细节、限制和实测结果见 PHASE3_VM_MODULES.md；Phase 2 交接历史移到 docs/history/PHASE2_HANDOFF.md。

## 2026-10-05 Phase 4：HUAB 与 Native/WASM

用户授权继续文件读写及外部模块加载。`.huab` 保存指令、最小声明和来源表，独立读取并验证后运行；不是 import 模块或自动缓存。
Native 使用 `.huam` 清单和同名动态库、opaque scalar C ABI v1；完整图通过检查后才装载和调用入口。
WASM 通过 Wasmtime 49.0.2 C API 校验数值接口，禁用 host imports/WASI，实例有 fuel/线性内存限制。
WASM 字节嵌入 HUAB；Native 只保存相对路径，部署需带动态库。Windows CLI 同目录需有 wasmtime.dll。
ABI/bytecode 独立提升为 1，spec 保持 0.1；这是当前版本合同，不宣称跨未来版本兼容。
公共标量扩展不意味着 extern/ref/ptr/struct FFI 或 Hua→WASM 后端已经实现。
不改变运算符与注释；不初始化 Git或写个人记忆。细节与验收见 PHASE4_ARTIFACTS_EXTENSIONS.md。
