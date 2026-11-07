# 2026-11-07 HUA-D038 P1 Unicode大小写（since26）

- 任务：按[六组提案](../designs/2026-11-07-p1-remaining-six-groups-proposal.md)C组实现`strings.upper/lower/casefold/equal_fold`。
- 身份：9eea2d8同工具链Release为基线（tests/build-c26-base）。数据：`tests/tools/gen_unicode_case.py`由Python 3.14.3（Unicode 16.0.0）生成`src/unicode_case_data.inc`。输入：`perf/stdlib/strings/25–26`。
- 正确性：`tests/unicode_case_tests.py`31项，三路径；全部Unicode标量（每行一个）的upper/lower/casefold与Python字节相同；415个词尾Σ上下文（手写＋固定种子随机）与Python相同；equal_fold两方向等于casefold比较；结果超16 MiB E4099。首轮失败是测试用`ﬃ`（3字节→3字节）触发不了16 MiB上限，改用U+0390（2→6字节）。Python不直接暴露Case_Ignorable，由`str.lower`的词尾Σ行为反推，已被上述对照覆盖。
- 性能（各7次）：手写小映射表0.149→原生0.025秒（6.0x），Hua/CPython 4.63x→0.78x，Hua/Lua 6.92x→1.19x（`win-stdlib-case-baseline.json`、`win-stdlib-case.json`）。原生已接近进程启动固定开销。首版基准的`equal_fold(text, text + "")`命中相等捷径，没有测到折叠，已改为与小写文本比较并重测。
- 体积：hua.exe 4,985,856→5,035,520字节（+49.7 KiB，提案估计约36 KiB，数据源文件84,538字节）。
- 回归：core 0.98–1.02x；stdlib 123项中位1.00x（`win-case-core-ab.json`、`win-case-stdlib-ab.json`）。低于0.96x的3项15轮复测： strings/09 0.97x encoding/03 0.98x math/05 0.96x。
- 自举：C2逐字节不变（f73869fa…，1,281,255,218步）。未跑全量CTest（六组完成后统一跑）。
