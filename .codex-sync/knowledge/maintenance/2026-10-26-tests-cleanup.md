# tests 旧产物清理（2026-10-26）

用户先询问tests产物是否可删，随后明确授权：“不再使用的帮我删除”。本次只做本地产物清理，不改变语言、标准库、自举与性能主线。

基线：main `776d5f8100f615ab9986f7d3feb8e3a1cc51ecd5`，开始时Git工作区干净。没有提交或推送。

## 范围与决定

- 保留tests/build、tests/build-selfhost、测试源码/夹具、当前安装版Hua，以及仍被审计工具引用的历史基线和输入。
- 历史运行数据先逐文件计算SHA-256，压缩为ZIP，再从ZIP解压读取计算SHA-256与原文件比较；所有归档通过后才执行删除。删除前逐项目录解析绝对路径，确认均位于C:/file/project/hua/tests内、没有Git跟踪文件或重解析点，并复核文件数量、大小、时间戳及ZIP SHA-256。删除使用PowerShell Remove-Item -LiteralPath。
- 旧C04实验已被默认实现替代、旧自举阶段输出已被后续阶段替代；需要复现旧实验时从归档恢复，不把旧CMake缓存用于增量构建。
- tests/artifacts/build/c04-performance仍被selfhost_pipeline_budget_tests.py引用，bo01-native-cpu-r2-20261016仍被audit_value_calls.py引用，故均跳过；BO-01/BO-04/BO-11冻结目录、layout与trace证据保留原路径。
- tests/build-prof、build-profile、build-bench及tests/__pycache__在本次开始时已不存在；不将它们算作本次清理成果。

## 实际结果

- 删除16个已归档旧目录、1个可再生缓存目录，共7882个文件。
- 原目录文件大小合计：734.79 MiB；16个ZIP合计：183.23 MiB。
- tests从2.202 GiB降至约1.667 GiB，扣除归档和清单后净释放约547.82 MiB。以上按文件长度求和，不是磁盘簇分配测量；随后生成的小型记录未计入这一快照。

## 已归档并移除的旧目录

| 原目录 | 原大小MiB | ZIP大小MiB | ZIP文件 |
|---|---:|---:|---|
| tests/build-selfhost-c04fix | 23.55 | 3.69 | tests__build-selfhost-c04fix.zip |
| tests/build-selfhost-f1 | 30.79 | 4.85 | tests__build-selfhost-f1.zip |
| tests/build-selfhost-p2 | 7.47 | 1.20 | tests__build-selfhost-p2.zip |
| tests/build-selfhost-p3 | 15.48 | 2.44 | tests__build-selfhost-p3.zip |
| tests/build-selfhost-p4 | 11.48 | 1.82 | tests__build-selfhost-p4.zip |
| tests/build-xfer | 1.35 | 1.35 | tests__build-xfer.zip |
| tests/artifacts/c04-direct-20261020 | 43.95 | 13.26 | tests__artifacts__c04-direct-20261020.zip |
| tests/artifacts/c04-r1-r2-20261020 | 120.27 | 26.02 | tests__artifacts__c04-r1-r2-20261020.zip |
| tests/artifacts/c04-symbolic-20261020 | 110.94 | 22.58 | tests__artifacts__c04-symbolic-20261020.zip |
| tests/artifacts/claude-transfer | 1.57 | 1.56 | tests__artifacts__claude-transfer.zip |
| tests/artifacts/claude-vm-perf | 2.73 | 0.68 | tests__artifacts__claude-vm-perf.zip |
| tests/artifacts/build/c04-origin-20261019 | 127.97 | 31.94 | tests__artifacts__build__c04-origin-20261019.zip |
| tests/artifacts/build/runtime-unified-20261019 | 116.68 | 38.85 | tests__artifacts__build__runtime-unified-20261019.zip |
| tests/artifacts/build/c04-call-20261020 | 92.76 | 27.51 | tests__artifacts__build__c04-call-20261020.zip |
| tests/artifacts/build/CMakeFiles | 14.07 | 2.78 | tests__artifacts__build__CMakeFiles.zip |
| tests/artifacts/build-python-off/CMakeFiles | 13.70 | 2.71 | tests__artifacts__build-python-off__CMakeFiles.zip |

缓存：tests/tools/__pycache__直接删除，Python可再生；无源码删除。

## 验证

- 在追加本次维护文档之前，2017个已跟踪文件SHA-256全部与清理前一致。
- 8736个保留构建/冻结基线文件SHA-256一致；安装版hua.exe与wasmtime.dll两文件SHA-256一致。
- 清理前后tests/build/hua.exe version均退出0，输出版本0.1.0-dev；quickstart均退出0，stdout均为Hua 42、10、3.5 3 1三行。
- 不涉及业务代码修改，未运行完整CTest、自举、性能基准，也未用历史测试冒充本轮验证。

## 本地证据与恢复

本地证据目录：tests/artifacts/cleanup-20261026/。plan.json保存边界与前置哈希；verified.json保存各ZIP和逐文件SHA-256；deletion.json保存实际删除目录；verification.json保存清理后核验和空间快照；archives/保存16个完整ZIP。该目录被Git忽略，归档仅在本机，普通Git推送不携带。

每个ZIP都保留从仓库根目录开始的原相对路径。在仓库根目录恢复某一组，例如：

```powershell
Expand-Archive -LiteralPath .\tests\artifacts\cleanup-20261026\archives\tests__build-selfhost-f1.zip -DestinationPath .
```

恢复前先核对对应ZIP SHA-256与verified.json一致。不要覆盖已有同名新产物。旧报告中原路径保持原文，通过本记录映射到压缩归档；不改写历史证据。

## 后续

当前HUA-D038主线及用户待选择事项不变。归档仅供历史复现，不属于默认运行/构建依赖；以后若清除归档本身，需要明确接受失去本机完整历史原件。
