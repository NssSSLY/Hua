# 尚未冻结的问题与规范冲突

这些问题保持开放；Phase 1/2/3/4 使用明确的实现边界，后续更改先修订规范和测试。

| 问题 | 原规范位置 | 当前处理 |
|---|---|---|
| 幂应高于 unary，但 EBNF 将 unary 放在 power 下层 | §16 / §61 | 遵循正式优先级；幂右结合，有结构测试 |
| 丢弃换行建议与 return 空值、语句边界冲突 | §60.4 / §61 | 保留 NEWLINE，按语法上下文处理 |
| IDENT 后 `{}` 可能是空 struct 或控制流块 | §10 / §61 | 条件头部将空 {} 视为 block；空 struct 可在括号内写 |
| 无初始化 array/map 示例 vs EBNF 要求 = | §8.1 / §9 / §61 | 本阶段要求初始化，未决定零值规则 |
| ref/mutref 与 &/&mut 等语法选型 | §19 / §55 | 支持已给出的 ref<T> 类型和 & 表达式语法；mutref 和 &mut 暂缓 |
| let 内部修改与部分示例冲突 | §5 / §9 / §62 | 已检查绑定、readonly view 和已知 receiver；动态/深层别名有运行时补充检查 |
| 方法 mut receiver：自动分析或显式签名 | §62.4 | 采用初期方案 B，推断 self 写入并传播已知方法修改效果；显式签名未冻结 |
| 数值宽度、混合提升、溢出与负数除法/取模 | §6 / §55 | 当前临时 int64/float64 并检查边界；普通混算须显式转换；// 为 int 向下取整，% 与之对应 |
| Slice 参数只读规则、Map 缺失键、GC 策略 | §55 | Slice 已默认只读并支持 mut 参数；Map/GC 仍后续决定 |
| const 编译期求值与多返回绑定 | §5.3 / §13 | 已实现标量 const 编译期求值；多返回执行/绑定和多赋值暂缓 |
| enum、panic、timeout、parallel/simd 严格性 | §18 / §55 | 登记相关保留词（panic/timeout 不新增关键词），未实现者报错 |
| 泛型调用、指针 cast、extern/FFI、安全引用有效性 | §17 / §19 / §29 | 仅解析 Result/ref/ptr 类型及 unsafe 边界；没有 FFI 或引用证明 |

本地源模块的 Resolver、内存缓存、pub 接口、别名与循环检测已实现，见 PHASE3_VM_MODULES.md。
`.huab` v1 完整程序文件、Native ABI v1 标量模块与 WASM 数值模块已实现，见 PHASE4_ARTIFACTS_EXTENSIONS.md。
选择导入/重导出、包配置/锁文件、HUA_PATH 等搜索策略、自动磁盘缓存、HUAB 模块导入和更丰富外部值/host API 仍未实现。
`interface`、`match`、`enum`、`async`、`await`、`spawn`、`taskgroup`、`parallel`、`simd`、
`extern`、`defer` 与 attribute 用明确的 Phase 1 未实现诊断拒绝；关键字登记不意味着执行能力。

## 已解决：注释与整除冲突

2026-10-05 用户已冻结 `#` 行注释、可嵌套 `#* ... *#` 块注释，以及文档标记 `##` / `#** ... **#` 和首行 shebang。
`//` 在所有位置只识别为整除；旧行首 `//` 注释和 `/* ... */` 规则已移除。
本项已移出未决表。文档提取工具仍未实现，标记已预留；Lexer/Parser 规则见 LEXER_PARSER.md。

其余当前限制和后续工作详见 PHASE2_SEMANTICS.md、PHASE3_VM_MODULES.md 与 PHASE4_ARTIFACTS_EXTENSIONS.md。

