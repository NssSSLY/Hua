# CPython对齐·调用路径（2026-10-23）

机器：另一台Windows x64机器（`E:\project\hua`），CPython 3.14.3；Lua 5.4.7按开发指南从lua.org源码构建（下载包sha256 9fbf5e28…与官方一致）。用户指定任务：“调用路径优化（03_calls：激活分配、参数向量、function_layout哈希查找），先剖析再改，验收按性能日志规则”。结论性记录在`docs/VM与自举性能改造分析.md`第0.2节“CPython对齐阶段·调用路径”。

## 身份

- 基线：main `f296e89`（P4标量快路径），工作区另含`src/vm.cpp`中一个原始NUL字节改为`'\0'`（同一字符值，只为让Git把该文件当文本）。基线二进制`tests/build-calls-base/hua.exe`（sha256 9ef04cbe…）。
- 剖析构建：`tests/build-prof`（Release，`-g -fno-omit-frame-pointer`，`BUILD_TESTING=OFF`）与`tests/build-prof/sampler.exe`（`tests/tools/native_profile/busiest_thread_sampler.cpp`）。
- 剖析输入：`perf/bench/03_calls.hua`改为fib(32)的副本（`tests/build-prof/calls32.hua`），`--execution-profile compiler`，81,065,288步。

## 剖析（改前，987个样本）

含子调用：`complete_return` 31.0%、`enter_function` 23.8%、`pop_arguments` 8.6%、激活环境`shared_ptr`删除器7.9%、`std::string`长串复制构造6.2%、`function_layout` 5.9%（其中murmur哈希3.1%）、`enforce_return` 5.8%、`operator new` 5.9%、`weak_ptr`的lock/release约3.6%。

两项固定浪费的原因：
- `Frame::site`是按值保存的`SourceSpan`，其`file_id`是源码完整路径（本机超过SSO长度），每次调用复制一次。
- `complete_return`每次返回都调用`function_layout(*function)`，在`unordered_map<const Node*, FunctionLayout>`中查表只为取返回类型，而`enter_function`进入时已经拿到该布局。

## 改动（保留A与B）

- (A) `Frame`新增`const FunctionLayout* callee`，`enter_function`压帧后设置；`complete_return`优先使用它（为空时回退原查表）。指针指向`unordered_map`节点，节点在rehash时不移动；`route->layout`早已依赖同一性质。
- (B) `Frame`新增`const SourceSpan* site_ref`与`site_span()`。只有分发循环的Call会传入route，其`op`是`f.code->instructions[pc]`的引用，与帧的`code`指针同属已加载字节码，所以只保存指针；`call()`协程路径（defer执行、`main`入口）仍保存自有副本。三个读者（返回类型错误span、分发越界`invalid`、错误“called from”上下文）都改为`site_span()`。
- 改后剖析（970个样本）：字符串复制与`function_layout`不再出现；`complete_return`含子调用26.4%。
- (D，已撤回) 分发调用时把实参移动进形参（`copy_value(Value&&)`对共享struct仍走复制路径，语义不变）。A+B与A+B+D对照15次，7个微基准0.99–1.02x，无可测收益，按规则撤回。

## 验收

- `perf/ab_bench.py`（基线对改后，9次交替）：01 1.00x、02 0.99x、**03 1.15x**、04 1.02x、05 1.00x、06 1.00x、07 1.03x、08 1.02x、09 1.02x；步数与`.expect`全部一致。`perf/results/win-parity-calls-ab.json`。`perf/results/compiler.huab`用基线二进制构建（本机，不入Git）。
- `perf/reference_bench.py`（同机同时段，CPython 3.14.3，改前/改后各7次）：03_calls 4.28→3.67倍，其余在噪声内（01 3.16→3.07、02 4.12→4.19、04 5.34→5.29、05 3.32→3.32、06 10.44→9.94、07 5.72→5.60）。改后结果`perf/results/win-reference-parity-calls.json`，改前结果`tests/build-calls-base/reference-base.json`（本机）。3.14比3.12快，所以这些倍数不能与原机P1–P4表对照。
- 补测（安装Lua后，参照两列各7次、非交替）：03_calls对CPython 4.31→3.70倍、对Lua 12.2→10.3倍；01与06的参照倍数在两次独立运行间大幅波动（同项A/B为1.00x），以A/B为准。改前带Lua结果`tests/build-calls-base/reference-base-lua.json`（本机），改后覆盖写入`perf/results/win-reference-parity-calls.json`。
- 自举回归：改后宿主构建C1（20.0秒）→ C2 195.2秒，sha256 f73869fa…、4,195,418字节，1,281,255,218步，与记录逐字节相同。`tests/build-calls-selfhost/summary.json`（本机）。
- 72项CTest：71项通过（340.7秒）；`archives_extensions`为HUA-D037已登记的本机测试问题（“第16字节写0”变异在本机产物上是空操作），与本改动无关。
- 定向测试：vm_slots、vm_observer、semantics_runtime、vm_type_contract、value_copy、enum_dispatch、container_execution、control_cleanup_execution、diagnostic_source、diagnostic_contract、value_lifetime、lifetime_contract、capability_contract全部通过。

## 未做与原因

- 激活环境的`shared_ptr`控制块与删除器中`weak_ptr<FrameArena>`的原子操作（约17%）：`run()`每次替换`frame_arena_`，环境可能比竞技场活得久，`weak_ptr`正是防护。要去掉它，需先证明环境寿命不超过竞技场，或改为侵入式计数，属单独设计。
- `pop_arguments`与形参按名字绑定：栈项可能是`LocalRef`，需要转成值；彻底解决依赖P5槽位化局部变量。
