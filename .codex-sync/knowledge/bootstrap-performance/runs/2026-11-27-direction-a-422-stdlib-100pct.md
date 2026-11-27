# 2026-11-27 方向 A 验收记录：32 内建模块 / 422 内建函数 100% 四路径全通

- **日期**：2026-11-27
- **基线**：`main@39c577d`
- **目标**：一次性收尾方向 A 全部三项缺口（维持 `HUAB format 33`，共享一次 `huaBin/` 固定点重建）：
  1. **A1（`--release` 原生 `async/spawn/await/taskgroup` + 9 项 `std.task`）**：在 `include/hua/native_rt.hpp` 新增 `hn::Task<T>` 模板包装、`Opaque<Tag>` `task_input`（活句柄跨 Task 边界触发 `E4104`）、`task_input_any` 与 `task_group_end`；在 `selfhost/native_cpp.hua` 开放 `async-body` 角色、自动探测 `uses_tasks()` 启用 `hn::run_main(..., true)` 纤程调度，并生成 `task_start`/`task_await`/`task_group_begin`/`task_group_end`（含 `defer`）与 `std.task` 9 项纤程挂起调用。
  2. **A2（19 项高阶回调标准库）**：在 `selfhost/types.hua` 新增 `check_callback()` 与 19 项回调签名/返回类型推导（`alg` 13 + `list` 3 + `heap` 2 + `maps.update` 1）；在 `selfhost/native_cpp.hua` 将 19 项回调直连 `include/hua/detail/native_algorithms.hpp` 的零装箱 C++20 模板（`hn::alg_*`、`hn::list_*`、`hn::heap_*`、`hn::map_update`），并在 `sync_from_value(List<T>&)` 中同步递增 `revision`。
  3. **A3（4 项 `std.simd`）**：在 `selfhost/parser.hua` `import` 路径段放开 `simd` 标识解析，使 `import std.simd as <alias>` 四路径全通。

## 自举不动点与验收结果

- **`huaBin/` 固定点重装配**（`build_step1_huabin.py`，临时 staging 目录验后已清理）：
  - `A2` (`hua-archive.exe`) SHA-256：`f1400b3344d911d68780320442a8e8a8deb71be6ee0d86673dc8f57f4952776b`
  - `C2 (16.540s) == C3 (125.465s, 794,922,023 步)` HUAB SHA-256：`58cc4035ffbc02c935c95ecfc13a80787f30935fe900d6b49c1b9661f9f0ee3f`（`6,093,611` 字节，`91,278` 条指令）
- **四路径 E2E 验收**（`python tests/step5_direction_a_tests.py --sdk huaBin`，耗时 `7.44s`）：
  - `Path 1 (hua run)`、`Path 2 (Host HUAB)`、`Path 3 (Selfhost HUAB)`、`Path 4 (Selfhost Native --release EXE)` **4/4 全过**，完整覆盖 `async fn`/`await`/`task.all`/`task.race`/`task.timeout`/`task.cancel`/`task.state`/`task.done`/`taskgroup`、全部 19 项高阶回调（含 `sorted_by_key` 稳定性、`list.sort_by`/`retain`、`heap.push_by_key`/`pop_by_key`、`maps.update`）与 `std.simd` 4 项。
- **CTest 回归**：`archives_extensions`（`17.42s`）、`module_contract`（`8.77s`）、`stdlib_contract`（`0.42s`）**3/3 全过**。
