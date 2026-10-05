# AST 与 SourceSpan

AST 是带枚举 NodeKind 的纯语法树，Node 包含 kind、span、text、拥有的 children。
它不依赖 Runtime，也不存储解释执行结果。AstPrinter 输出稳定 S-expression，字符串会转义。

## 子节点约定

| 节点 | text | children 顺序 |
|---|---|---|
| Program/Block | 空 | 源码顺序的语句 |
| Let/Var/Const | 绑定名 | 可选类型，然后初始化表达式 |
| Function | 名称（方法含 owner.），pub/unsafe 标记 | Parameter*，可选 ReturnTypes，Block |
| Parameter | 名称、可选 mut | 可选类型 |
| ReturnTypes | 空 | 类型列表 |
| Struct | 名称、可选 pub | Field* |
| Field | 字段名 | 类型 |
| Import | 点分路径 | 可选别名 Name |
| If | 空 | 条件、then Block、可选 else Block/If |
| While | 空 | 条件、Block |
| For | 空 | 1 或 2 个绑定 Name，迭代对象/Range，Block |
| Range | .. 或 ..= | start、end、可选 step |
| Return | 空 | 返回表达式列表（可空） |
| Break/Continue | 空 | 无 |
| ExpressionStatement/Unsafe | 空 | expression/Block |
| Integer/Float/String/Boolean/Name | 字面文本/解码字符串/名称 | 无 |
| Nil/Omitted | 空 | 无；Omitted 只用于缺省切片端点 |
| Unary/Update | 运算符 | 操作数 |
| Binary/Assignment | 运算符 | 左、右 |
| Call | 空 | callee、arguments* |
| Index | 空 | target、index |
| Slice | 空 | target、start 或 Omitted、end 或 Omitted |
| Member | 字段/方法名 | target |
| Array | 空 | 元素* |
| StructLiteral | 类型名，可含模块路径/别名 | FieldInit* |
| FieldInit | 字段名 | value |
| TypeName | 可点分名称 | 无 |
| SliceType/MutableType/OptionalType | 空 | 类型 |
| ArrayType | 长度字面文本 | 类型 |
| GenericType | ref/ptr/Result/map | 类型参数列表；map 为 key,value |

每个节点保留文件、start/end offset、line/column。
范围为半开区间，偏移基于 Source 规范化后的 UTF-8 字节；行列从 1 开始。
括号被折叠到其表达式节点，span 仍覆盖括号。绑定名的精细符号解析由下一阶段扩展。
Parser 首错即停止，不输出成功 AST；诊断包含文件位置、源码、原因和箭头，明确时提供 help。
Parser 递归/单段长链上限 192，AST 高度上限 384，超限报告 E2008。
深度和长链限制为开发安全边界，后续可改为迭代实现以支持更大合法程序。

## 模块链接与执行

`ast` 保持原始语法 AST 输出，`geo.Point{x:1}` 的 StructLiteral.text 为 `geo.Point`。
ModuleLoader 克隆并链接 AST：去除已解析 Import、为模块顶层名字添加内部前缀、
将公开导入引用绑定到实际声明，同时保留各节点原文件 SourceSpan。
链接 AST 不覆盖原始文件/AST；前端不负责文件 I/O、pub 验证或模块初始化。
SemanticModel 和内存 Bytecode 使用链接节点的生命周期，执行 API 需保持 ModuleLoader 存活。

## 外部接口与持久化

ExternalInit 是 ModuleLoader 生成的内部节点，text 为外部模块 ID，children 为空；源码 Parser 不产生此节点。
Native/WASM Function 增加 external 标记，保留 Parameter/ReturnTypes 和空 Block，仅用于类型检查和声明登记。
BytecodeCompiler 为其发出 ExternalInit 初始化指令，不编译空函数体；两个执行器通过 ExternalRegistry 调用真实导出。
HUAB 读取只重建最小 Function/Struct 声明及签名，不重建函数体；Code 指针关联到 BytecodeImage 拥有的声明。
函数体执行仍为指令路径；Source 文本只用于位置诊断。BytecodeImage 必须比其 model/bytecode 执行活得更久。
