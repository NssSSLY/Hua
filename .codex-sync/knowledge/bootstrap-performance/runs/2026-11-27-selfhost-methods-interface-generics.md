# 2026-11-27 方向 B1/B2/B3 验收：自举前端与原生后端补齐固有方法、`interface` 与显式用户泛型

## 任务与范围
- **基线**：`main@269e76c`（方向 A 422/422 四路径 100% 全通基线）。
- **目标**：在不升级 HUAB 格式版本（保持 `HUAB format 33`）与不新增 C++ 运行时库的前提下，为 `selfhost/` 完整补齐（涵盖 Selfhost HUAB 与 Selfhost Native `--release`）：
  1. **B1 固有方法**：`struct`/`enum` 上 `fn Type.method(...)`、隐式 `self` 绑定、传递 `mutating` 不动点推断、只读接收者拒写 `E3003` 与跨模块 `pub` 边界 `E5003`。
  2. **B2 结构化 `interface`**：`interface Name { [mut] fn m(...) ... }`、结构化满足关系与只读/可变签名核验、`I{}` 构造拒绝 `E3004`、`bytecode`/`archive` 抽象原型与接口结构元数据，以及 `native_cpp.hua` 基于 `hn::Interface<Tag_...>` 与 `switch(v_self.object.tag)` 的动态派发代码生成（支持值装箱、接口转换、`mut` 借用与 `clone`）。
  3. **B3 显式用户泛型**：`struct Box<T>`、`fn id<T>(...)`、嵌套 `>>` 右尖括号拆合（`pending_gt`）、后缀显式特化 `f<T>(...)` / `Box<T>{...}`、新增 `selfhost/generics.hua` 在 `modules.hua` `collect()` 前执行 AST 级单态化（上限 128、`E3010`、跨模块嵌套泛型别名前缀重映射与后序 DAG 新根节点生成；无泛型源码零克隆快路径）。

## 主要改动
- **自举编译器（16 模块）**：
  - `selfhost/ast_pool.hua`：新增 `Interface(string)`、`GenericParameters`、`Specialize` 变体与形状规则；允许 `Function` 2 或 3 子节点。
  - `selfhost/parser.hua`：开放 `fn Type.method(...)`、`interface` 声明、`struct/fn <T, U>`（重复参数 `E2006`）、`pending_gt` `>>` 拆合及后缀 `Specialize` / 特化结构体字面量。
  - `selfhost/generics.hua`（新增）：独立 AST 单态化模块（`needs_specialize` 零开销快路径 + `specialize_all`）。
  - `selfhost/modules.hua`：接入 `generics.specialize_all`、方法所属类型与 `self` 参数挂接（附着于 `Parameters` 节点避免索引冲突）、`Interface` 符号收集与可见性。
  - `selfhost/types.hua`：新增 `infer_mutating()` 不动点、`build_interface_relations()`、`require()` 接口满足判断、方法成员查找（`E5003`/`E3003`）、`Interface` 语句放行、抽象方法直接调用与接口直接构造拒绝（`E3004`）。
  - `selfhost/lowering.hua`：下沉 `self`、`interface-layout`、`abstract` 及 `Core.mutating`/`Core.relations`。
  - `selfhost/bytecode.hua`、`selfhost/bytecode_verify.hua`、`selfhost/archive.hua`：写出并验证 `mutating`、`interface` 结构、`abstract` 原型与方法内 `self`。
  - `selfhost/native_cpp.hua`：固有方法 `v_self` 传参（排除 `enum-constructor`）、`hn::Interface<Tag_...>` 映射、`emit_abstract()` 动态派发、`contextual_expression()` 接口装箱/转换与 `emit_split` 跨模块依赖收集。
- **验收测试**：新增 [tests/selfhost_methods_interface_generics_tests.py](../../../../tests/selfhost_methods_interface_generics_tests.py)。

## 固定点与实测结果
1. **自举固定点重装配（`python tests/tools/build_step1_huabin.py`）**：
   - `G1 == G2` PE SHA-256：`d9dafe29a269571778fc838e312f56cd1054180f2dd2c3a9fafa3dc9f7cc29c5`（`build-G1` `306.041s`，`build-G2` `17.674s` 缓存全命中）
   - C++ 生成逐字节固定点 SHA-256：`345d58169956c447ddee515e4f9e00421bc0defb47300334f3b1a63dc988309e`
   - `A2` (`hua-archive.exe`) SHA-256：`cc0200cb4d5cd891a001b1bfb4e32a3cdac9d53bf9a7c6e811eaee801c8b0d21`（`68.163s`）
   - `C2 == C3` HUAB 逐字节固定点 SHA-256：`669d061eed4a16cd33b221502b5b5b05b5b987be03ba73cee44552ae91cbc67f`（`6,506,831` 字节，`97,283` 指令，`CRC32 1063250994`，`C2 19.937s`、`C3 158.688s` / `848,906,965` 步、`peak_calls 53`）
2. **B1/B2/B3 四路径与负例验收（`python tests/selfhost_methods_interface_generics_tests.py --sdk huaBin`）**：
   - `b1_methods_basic`（`struct`/`enum` 固有方法、传递 `mutating`、值复制 vs 就地修改、跨模块 `pub`）：**4/4 路径通过**
   - `b2_interface_dispatch`（跨模块结构化 `Shape`/`Scalable`、`mut fn`、返回接口、`mut Scalable` 借用接口与具体结构体、`clone(iface)`）：**4/4 路径通过**
   - `b3_user_generics`（跨模块 `Box<T>`/`Pair<K,V>`/`id<T>`/`wrap<T>`/`swap_pair<A,B>` 与嵌套 `Box<Box<T>>` `>>` 拆合）：**4/4 路径通过**
   - 6 项负例诊断（`E3003`、`E5003`、`E3004`×2、`E3010`、`E2006`）：**6/6 通过**
   - 总耗时：`10.51s`
3. **回归验证**：
   - `python tests/step5_direction_a_tests.py --sdk huaBin`：**4/4 路径通过**（`7.56s`）
   - `ctest --test-dir tests/build -R "^(stdlib_contract|archives_extensions|module_contract)$"`：**3/3 通过**（`26.85s`）

## 结论
- `selfhost/` 现已完整覆盖固有方法、`interface` 与显式用户泛型，四路径语义一致且达成 16 模块 `G1 == G2` / `C2 == C3` 逐字节不动点；自举编译自身因 `needs_specialize` 快路径零克隆开销，步数与编译耗时保持平稳。
