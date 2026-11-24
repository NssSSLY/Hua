# 2026-11-24 C04 分解式短 ID 来源池、函数操作表与自底向上 SCC 求解实测

## 1. 任务与输入身份

- **目标**：按四阶段方案改造自举编译器 `selfhost/capabilities.hua` 的 C04 能力流分析（阶段 1 因果重访表与收益上界 → 阶段 2 只读操作表与按需展开 → 阶段 3 分解式短 ID 来源与通道订阅 → 阶段 4 同输入双端验收与 C2=C3 固定点）。验收建议目标：冻结自身输入 C04 耗时降至基线 $\le 1/3$，完整编译墙钟 $\ge 2\times$，且原生与 VM 双端同时受益、产物逐字节一致。
- **代码基线**：分支 `perf/vm-register-r1`，`HEAD = 349b79c`。
- **冻结 14 模块输入**（`tests/artifacts/c04-revisit-20261124/frozen/`）：`archive.hua`、`archive_main.hua`、`ast_pool.hua`、`bytecode.hua`、`bytecode_verify.hua`、`capabilities.hua`、`catalog.hua`、`lexer.hua`、`lowering.hua`、`modules.hua`、`parser.hua`、`signatures.hua`、`type_rules.hua`、`types.hua`。
- **冻结输入标准 HUAB 身份**：`4,532,637` 字节，`crc32 = 558414254`，`sha256 = db6dcfcc3d8f0ad1...`。
- **更新后生产源码**：修改 `selfhost/capabilities.hua`，新增 `selfhost/origin_pool.hua`（更新后自举闭包为 15 模块）。未改动任何 C++ 运行时或宿主编译器代码。

## 2. 阶段 1：冻结基线与 Top-3 热点因果重访表

原始证据见 `tests/artifacts/c04-revisit-20261124/ordinary-baseline/summary.json`、`causal-report.txt` 与 `causal-analysis-summary.json`。

- **普通无埋点基线（冻结 14 模块）**：
  - 原生（`huaBin/lib/compiler/hua-archive.exe`）：墙钟 `52.256s`、CPU `51.797s`、峰值 `175.38 MiB`。
  - VM（`huaBin/bin/hua.exe run huaBin/lib/bootstrap/compiler.huab --execution-profile compiler --report-budget`）：墙钟 `266.516s`、CPU `263.625s`、峰值 `963.85 MiB`、总步数 `1,260,051,151`。
- **独立观察器步数拆解（`c04_vm_body_probe.exe`）**：
  - C04 占 `934,623,652 / 1,260,051,151` 步（`74.17%`），非 C04 阶段合计 `325,427,499` 步（`25.83%`），7 轮，`work = 181,093`。
  - 全部 288 函数首次访问仅 `42.54M` 步（`4.55%`），278 次重访占 `829.28M` 步（`88.73%`）。
  - 纯无效重访（输出摘要与错误完全不变）：104 次、`134.56M` 步（`14.40%` C04）→ **仅消除无效重访上界仅 `1.17×`，不足以达到 `3×` 目标**。
  - **主要矛盾**：`48.59%` 的 C04（`454.13M` 步，49 次重访）来自仅 `origins` 增长的宽来源重访，且集中在 Top-3 函数（合计 `71.77%` C04）：
    1. `capabilities.statement`：`373.51M`（`39.96%`，6 访：`2.59M → 23.88M → 81.25M → 85.19M → 90.14M → 90.46M`；第 6 访因自递归 `orig(809→819)` 触发 90.46M 纯无效重访）。
    2. `capabilities.expression_body`：`149.90M`（`16.04%`，6 访：`3.12M → 37.72M`）。
    3. `capabilities.analyze`：`147.43M`（`15.77%`，5 访：`2.11M → 42.89M`）。
  - 独占热点：`instantiate_view_at` `33.00%`（`308.4M`）、`merge_view` `18.62%`（`174.0M`）、`note_prefix` `5.38%`、`storage_aliases` `4.41%`、`put` `3.77%`。

## 3. 逐轮演进与否决记录（均核对冻结 14 模块 HUAB 逐字节一致）

| 轮次 | 核心改动 | 原生墙钟 / 峰值 | VM 墙钟 / 步数 / 峰值 | 结论 |
|---|---|---:|---:|---|
| 基线 | `selfhost/capabilities.hua`（`349b79c`） | `52.256s` / `175.38 MiB` | `266.516s` / `1,260.05M` / `963.85 MiB` | 对照基准 |
| Iter 1 | `summary` 规范化 `!` 来源、预编译写/需求表、调用点 `need_fields` 与 `sub_ret_fields` | `42.993s` (`1.22×`) / `208.5 MiB` | `230.649s` (`1.16×`) / `1,086.37M` (`1.16×`) / `1,030.9 MiB` | 方向正确但 `View.origins` 仍为宽字符串 `map`，未达标 |
| **Iter 2（否决）** | `View.oid` 统一短 ID 池 + 纯 Hua `List` 开放寻址哈希表 + `retag_local` 逐原子展开 | `37.699s` (`1.39×`) / `288.5 MiB`（单编 `capabilities.hua` 达 `3.31×`） | `288.711s` (**`0.92×` 退化**) / `1,261.57M` (`1.00×`) / `1,328.0 MiB` | **否决**：`retag_local` 仍展开 56K 集合，纯 Hua 哈希与 `intern`/`sort_ints` 吞噬 `440M` 步并推高内存 |
| Iter 3 | **50-bit 分解式 `View.oid`（`poid` + `lsite` + `loid`）**零展开 O(1) `retag_local`、紧凑有序对 `lmaps: List<[]int>`、`pure_scalar` 长度快筛、`note_prefix` 单段快筛、按类型 `alias_gen`、**Kosaraju 自底向上 SCC 局部不动点求解** | `19.923s` (`2.62×`) / `135.04 MiB` | `150.713s` (`1.77×`) / `766.14M` (`1.64×`) / `921.09 MiB` | 双端大涨、内存反降；剖析显示纯 Hua `List` 哈希表仍占 ~85M 步、非叶写清 `alias_cache` 仍占 66.8M 步 |
| Iter 4A | `origin_pool` 改用内建标量 `map[string]int`/`map[int]int`、非叶写全量按类型推进 `alias_gen`、`note_prefix` 复用 `@types` O(1) 去重、按字段返回需求 `demand`（`"!"`/`""`/`"#@field"`）、纯标量 `pure_fast` 调用快径 | `19.242s` (`2.72×`) / `136.72 MiB` | `154.034s` (`1.73×`) / `643.80M` (`1.96×`) / `920.49 MiB` | C04 步数降至 `318.4M`（`2.94×`），但 183K 无界活跃 map 条目增加 VM GC 追踪导致墙钟未降 |
| **Iter 4B / 最终落盘** | **50-bit `View.oid` 内联单元素编码**（`poid>=131072`、`loid>=1048576`，免除 ~3 万堆 `map` 与迭代器）、有界 `im`/`sm` 缓存、**阶段 2 只读模块操作表 `UnitPlan`（`[]NodeOp`）**与预缓存 `std_sigs`、`invoke` 内 `batch_aliases`、精简 `struct State` | **`16.822s` (`3.11×`) / `128.22 MiB`** | **`130.835s` (`2.04×`) / `624.77M` (`2.02×`) / `925.83 MiB`**（Iter 4B 同测 `123.245s` = `2.16×`） | **放行落盘 `selfhost/`** |

## 4. 最终双端验收结果（最终 `selfhost/` 版本）

原始证据：`tests/artifacts/c04-revisit-20261124/final-acceptance/`。

### 4.1 冻结 14 模块普通无埋点双端对照

| 指标 | 基线 | 最终候选（`selfhost/`） | 变化 |
|---|---:|---:|---:|
| 原生整次编译墙钟 | `52.256s` | **`16.822s`** | **`3.11×`** |
| 原生进程 CPU / 峰值 RSS | `51.797s` / `175.38 MiB` | **`16.031s` / `128.22 MiB`** | `3.23×` / **`-26.9%`** |
| VM 整次编译墙钟 | `266.516s` | **`130.835s`**（阶段对照同机 `110.941s`） | **`2.04×`** |
| VM 进程 CPU / 峰值 RSS | `263.625s` / `963.85 MiB` | **`126.547s` / `925.83 MiB`** | `2.08×` / **`-3.9%`** |
| VM 总预算步数 | `1,260,051,151` | **`624,770,564`** | **`2.02×`** |
| VM C04 步数（总步数 − 325.43M 非 C04） | `934,623,652` | **`~299,343,065`** | **`3.12×`**（降至 `32.0%`） |
| 冻结输入 HUAB SHA-256 | `db6dcfcc3d8f0ad1...` | **`db6dcfcc3d8f0ad1...`** | **双端逐字节一致 + `hua check OK`** |

### 4.2 独立副本 C04 阶段独占墙钟对照（冻结 14 模块，`std.fs.write_text` 时间戳，未混入普通计时）

原始数据：`tests/artifacts/c04-revisit-20261124/final-acceptance/phase-ab.json`。

| 后端 | 基线 C04 / 非 C04 / 合计 | 候选 C04 / 非 C04 / 合计 | **C04 阶段加速比** |
|---|---:|---:|---:|
| 原生（`hua build --release` 同工具链） | `62.608s` / `7.164s` / `69.772s` | **`9.210s`** / `5.928s` / `15.139s` | **`6.80×`**（降至 `14.7%`） |
| VM（`hua run --execution-profile compiler`） | `336.173s` / `41.742s` / `377.914s` | **`73.469s`** / `36.846s` / `110.315s` | **`4.58×`**（降至 `21.9%`） |

两项 C04 耗时均远低于“降至基线三分之一”的验收目标，且如今非 C04 阶段（`325.4M` 步，占 VM 总步数 `52.1%`）已首次超过 C04（`299.3M` 步，`47.9%`）。

### 4.3 正确性与代际固定点验收

1. **A07 / C04 完整差分套件**（`tests/capability_c04_tests.py --unbounded --compatibility --subset-probe --type-oracle --extra-cases tests/data/c04_call_cases.json --execution-probe`）：**`206 / 206` 全部通过**（`3,670` 项检查，`0` 失败），覆盖 AST / VM / 无源码 HUAB / SB1 四路径一致、与独立 C++ 类型预言机逐节点可写性一致、以及全部负例精确错误码与 `line:col` 一致（涵盖只读别名、跨函数写、多返回、递归、`defer`、`record_copy`、强/弱更新、32 轮保守回退与预算边界）。
   - 注：使用 `--unbounded` 仅放宽测试脚本的 60s 子进程监督时限（宿主 C++ `hua check` 在本机对冻结旧版 `capabilities.hua` 亦需 `72.3–73.4s`，新版为 `80.5s`，属既有 HUA-D036 类宿主前端时限），编译器自身全部预算限制保持不变。
2. **反向依赖单元测试**（`tests/capability_dependency_tests.py`）：AST / VM / build / source-free HUAB 全部通过（`9` 项断言 × `3` 路径）。
3. **更新后 15 模块 `selfhost/` 完整自举固定点 `C1 → C2 → C3`**：
   - `C1`（原生 `c1-archive.exe`）编译更新后 15 模块 `selfhost/archive_main.hua` → `C2`：`17.185s`，峰值 `139.17 MiB`，`sha256 = 566b7a698e394c9f...`，`hua check OK`。
   - `C2`（VM `hua run c2-selfhost.huab --execution-profile compiler`）编译同一 15 模块 `selfhost/archive_main.hua` → `C3`：`166.628s`，峰值 `1,006.21 MiB`，`682,337,327` 步，`sha256 = 566b7a698e394c9f...`，`hua check OK`。
   - **`C2 == C3` 逐字节完全一致**。

## 5. 已知边界与后续事项

- 三项直接以正则抽取 `selfhost/capabilities.hua` 私有字段 `View.origins: map[string]bool` 的白盒测试（`tests/capability_instantiation_tests.py`、`tests/capability_metadata_tests.py`、`tests/capability_seed_tests.py`）尚未适配新的 `origin_pool` 私有存储表示（`tests/capability_fixture_support.py`），后续若纳入全量 CTest 需同步适配 `capability_fixture_support.py`。
- 本轮未更新 `huaBin` 预装发行包内二进制（仍为旧版基线），未提交或推送 Git。
