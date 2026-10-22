# C04 符号表示、稀疏覆盖及组成版本试验

显式实验覆盖文件，默认selfhost/capabilities.hua保持原字节。最终语义对照通过，但真实C04管线步骤+4.44%/+6.51%、内存增加，未放行默认。放行状态以项目概览与本轮过程为准，不把私有测试通过当作自举成功。将本文件复制到同源selfhost目录的capabilities.hua位置构建，相邻导入模块必须来自对应快照。

- 来源：不可变基底＋显式投影操作，保留空操作、512重置及中间碰撞顺序；不是完整interned DAG。
- 写效果：发布一次解析owner/path，保持顺序与调用前实参快照。
- 覆盖：稀疏字段覆盖＋精确原字段键遮罩＋保守拓扑标记；覆盖优先于遮罩，批次分离一次，完整替换/结束清理句柄。
- 版本：returned/writes/required/control单独精确比较；来源、字段存在性和根能力参与失效。
- 函数体：写/要求/控制相同且各调用返回在原实参上精确相等，才复用节点事实与首错；逻辑work保持，可能跨预算时执行原遍历。

私有命令：python tests/capability_symbolic_tests.py tests/build/hua.exe . tests/artifacts/<新目录>。完整C04对照须同源覆盖、--execution-probe、--child-seconds 60 --prepare-seconds 120及冻结参考组件。没有绕过身份检查的默认开关。

[过程与失败](../../../.codex-sync/knowledge/bootstrap-performance/runs/2026-10-20-c04-symbolic.md)。原始日志不随Git同步；不改历史、不提交推送。
