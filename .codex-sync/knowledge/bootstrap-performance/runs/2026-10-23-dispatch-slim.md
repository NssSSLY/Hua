# CPython对齐·分发循环瘦身（2026-10-23）

机器：另一台Windows x64机器（`E:\project\hua`），CPython 3.14.3，Lua 5.4.7（按开发指南构建）。用户指定任务：“分发循环瘦身：逐指令的循环条件/越界检查、每步pump_pending、步数计数、LocalRef的shared_ptr计数，保持语义与步数不变，先剖析再改”。结论性记录在`docs/VM与自举性能改造分析.md`第0.2节“分发循环瘦身”。

## 身份

- 基线：调用路径构建（未提交，在main `f296e89`之上），`tests/build-dispatch-base/hua.exe`（sha256 4694ac39…）。
- 每项的中间二进制：`tests/build-dispatch-base/{a,ac,ab,ad}/hua.exe`（本机，不入Git）。

## 剖析

`tests/build-prof`，01_loop放大为3000万次迭代（1309个样本）。分发循环自身50%；用带内联与行号的符号化（scratch脚本，按叶样本统计）定位：
- vm.cpp:412–414（循环条件`frames_.size()`、pc越界检查、取`Instruction`）约16%；
- vm.cpp:419–423（轮询计数、`pump_pending`、步数计数与上限比较）约14%；
- `shared_count.h`（内联的`shared_ptr`计数）6.7%。
02_arith中分发自身40.6%；P5相关部分见0.2“P5实施前剖析”。

## 四项（各自单独构建，与上一项或基线做11次交替A/B）

| 项 | 改动 | 01_loop | 02_arith | 处理 |
| --- | --- | --- | --- | --- |
| (a) | `LocalRef`去掉`shared_ptr<Environment>` owner；`StoreMulti`构造`Location`时用帧当前环境作owner | 1.05x | 1.03x | 保留 |
| (c) | 步数上限在`dispatch()`入口读一次（`context_`只在`run()`前后切换） | 0.99x | 1.00x | 撤回 |
| (b) | `pump_pending`按（上下文、文件作用域）缓存标志指针，两者仍每步比较；`dispatch`局部变量，每次进入重置 | 0.87x | 0.95x | 撤回 |
| (d) | 帧缓存指令数组首地址与条数，取指与跳转越界检查改用它 | 1.01x | 1.01x | 撤回（噪声内） |

(b)变慢的原因判断：`dispatch`是协程，其局部变量位于堆上的协程帧，缓存额外增加了每步的读写与分支。(c)(d)没有收益，说明编译器已把这些量保持在寄存器或缓存中，剩余成本主要是访存（336字节Instruction）与variant构造析构。

(a)语义论证：`LocateName`经帧当前环境链解析绑定；绑定所在作用域包住整条赋值语句，右侧求值只会进出更内层作用域，所以在`Store`/`StoreMulti`消费栈项之前，帧的环境链一直让绑定存活。迟到绑定（`materialize_scope`）只新建子环境，父链仍持有原环境；`Unwind`与错误会在读取前丢弃栈项；绑定存储不搬移（槽位为定长数组，map节点稳定）；GC不扫描`LocalRef`。`LocalRef`只在vm.cpp三处出现（创建、`Store`、`StoreMulti`）。

## 验收（最终只含(a)，相对基线）

- `perf/ab_bench.py` 9次交替：01 1.09x、02 1.04x、03 0.98x（噪声，fib不走LocalRef）、04 1.00x、05 1.01x、06 1.01x、07 1.01x、08 1.00x、09 1.01x；步数与`.expect`全部一致。`perf/results/win-parity-dispatch-ab.json`。
- `perf/reference_bench.py`（改前/改后各7次，非交替）：01对CPython 3.09→2.92倍、对Lua 13.2→12.5倍，其余在噪声内。改后`perf/results/win-reference-parity-dispatch.json`，改前`tests/build-dispatch-base/reference-base.json`（本机）。
- 定向测试：vm_slots、vm_observer、semantics_runtime、vm_type_contract、value_copy、enum_dispatch、container_execution、control_cleanup_execution、diagnostic_source、diagnostic_contract、value_lifetime、lifetime_contract、capability_contract、task_runtime、task_resume_single、task_resume_migration、task_resume_public、concurrency、language_structure全部通过；(b)期间另跑file_resource_runtime、file_stdlib、resource_runtime通过。
- 自举回归：C2与记录逐字节相同（sha256 f73869fa…，4,195,418字节），1,281,255,218步，204.9秒。`tests/build-dispatch-selfhost/summary.json`（本机）。
- 72项CTest：71项通过（380.0秒）；`archives_extensions`为HUA-D037已登记的本机问题。

## 结论

逐指令固定开销中，现有结构内能拿到的只有引用计数这一项。其余来自协程帧、336字节`Instruction`的访存和144字节`Value`的variant构造析构，需要P7（紧凑指令）与P6（紧凑Value）解决。
