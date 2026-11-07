# HUA-D038 其余P1首批：Set对称差与64位整数工具

状态：**2026-11-04用户确认按推荐方案实施**，旋转次数0–63、越界E4003；其余按本提案。实现与验收状态见后续结果记录。

## 1. 已定稿接口（since21/HUAB21）

| 函数 | 参数 | 返回 | 推荐规则 |
|---|---|---|---|
| sets.symmetric_difference | a map[K]V1, b map[K]V2 | map[K]bool | 只在a或b之一存在的键；K限bool/int/string且两边相同，V可不同。沿用Set现有键集合合同，false/nil值仍算成员 |
| math.popcount | x int | int | 64位二进制补码中1的个数，结果0–64；popcount(-1)=64 |
| math.leading_zeros | x int | int | 从最高位起连续0的个数；零返回64，负数返回0 |
| math.trailing_zeros | x int | int | 从最低位起连续0的个数；零返回64，min_int返回63 |
| math.rotate_left | x int, n int | int | 位模式循环左旋，越出的位回填低位；结果按64位有符号补码解释 |
| math.rotate_right | x int, n int | int | 位模式循环右旋，越出的位回填高位；结果按64位有符号补码解释 |

已确认旋转次数0–63，0返回原值，负数或大于63为E4003。这样与Hua现有位移次数边界一致，但旋转允许最高位进入符号位，不沿用左移的溢出错误。未采用的备选：任意int次数取模64（含负数等价反向旋转）。保持提案历史，不作为当前合同。

例：popcount(-1)=64；leading_zeros(1)=63；trailing_zeros(8)=3；rotate_left(1,63)=min_int；rotate_right(1,1)=min_int；rotate_left(min_int,1)=1。

全部math参数须为默认int，不隐式接收i32/u64/float；静态类型错误沿用E3004，动态参数类型错误E4003。六个接口都是纯函数，整数返回值无需Result。容量和执行取消仍为运行时诊断。

Set结果新建可写map，所有值为true；输入不变、允许只读输入，结果与两个输入独立。结果超过100万条为E4099，工作上限3200万，每1024单位检查Task取消。只使用已有键的排序比较，不复制输入值。

## 2. 最小实现方案及影响

- collections_stdlib.cpp已有两棵有序树同步遍历：对称差保留单边键、丢弃公共键，O(|a|+|b|)遍历，沿用输出尾部hint插入与容量/取消检查；不组合两次difference再union，避免中间map。
- algorithm_stdlib.cpp数学分派复用现有默认int检查；用C++20 bit_cast<uint64_t>、popcount/countl_zero/countr_zero/rotl/rotr处理位模式，旋转结果bit_cast回int64_t，避免有符号移位溢出和实现定义转换。
- stdlib.cpp登记6个since21函数；Set泛型签名沿用现有类型特化和sema检查。archive reader支持21，writer按实际引用选择since，旧功能仍保持原格式；已有20不能静默加载新函数。
- 暂停中的selfhost签名/格式同步沿用L0-4待办，候选实施后应记9–21，不把宿主新增接口说成Hua前端覆盖。

## 3. 先有基线的实际结果

源码基线HEAD、运行时SHA和逐次计时见本文件旁的2026-11-04-p1-set-bit-tools-baseline.json。当前HEAD为12a0c23，先从当前源码重建hua（成功），再冻结exe与DLL。三轮同机顺序测量，均与.expect一致：

| 手写工作负载 | Hua中位秒 | CPython中位秒 | Lua中位秒 | 校验输出 |
|---|---:|---:|---:|---|
| 5000键、半重叠、20次Set对称差 | 0.237490 | 0.037226 | 0.016404 | 100000 |
| 20000整数的计位/前后零与双向旋转 | 1.055419 | 0.160642 | 0.038302 | 767797959 |

Python3.14.3、Lua5.4.7。均为手写同算法基线，含进程启动和Hua前端；不是新增原生接口的性能或成熟语言原生函数排名。新接口实施后增加原生版基准及对应参考，保留手写版以对照收益；既有core A/B再检查全局影响。

准备失败保留：新增Hua夹具首次误用分号，解析即停；随后旋转手写表达式触发检查左移的E4002，改为逐位构造；位复合赋值|=不支持，改为显式赋值。三次均已定因并只修新增夹具；没有产品代码变更。最终两个工作负载均完成三轮三语言检查。

## 4. 验收计划（实际结果见2026-11-04-p1-set-bit-tools运行记录）

1. AST解释器/源码VM/移除源码后的HUAB三路径：零、-1、min/max、每个单置位、确定性随机输入；每个旋转次数、双向逆变换，用独立Python64位oracle核对。检查n=-1/64和极值的错误合同。
2. Set覆盖bool/int/Unicode字符串键、两边同存储、全重叠/不相交/空、false/nil与不同V、只读输入、结果可写及输入不变；以手写键集合oracle核对。
3. 负例核对不同K、非默认int、错误个数和动态参数；HUAB21独立读取、降为20拒绝及旧接口格式保持。容量和取消以边界用例核验，避免每例百万级循环。
4. 本批专测、已有collections_p0/math_encoding_p0、档案/运行时相关回归；新增原生基准与CPython/Lua同机对照、既有core交替A/B。超时或稳定退化先定因，全部记录保留。

## 5. 后续P1顺序建议

此批结束后Set对称差组可关闭，math组仍部分完成。随后先为checked/wrapping/saturating整数运算及双曲函数写精确提案，再做strings center/码点与encoding基础；Unicode大小写需先定数据版本，缓冲I/O需先定资源/取消/部分读取合同。模板/JSON构造/命令行与子进程按原HUA-D005/D022单独切片，避免新增语言设计混入这一批。

## 6. 对照依据

- [Go math/bits](https://pkg.go.dev/math/bits)：64位零输入计前导/尾随零返回64。
- [Rust i64位方法](https://doc.rust-lang.org/std/primitive.i64.html#method.rotate_left)：固定宽度位模式旋转，次数按位宽循环；是备选方案的参考，不声称推荐0–63与它完全相同。
- [Python set.symmetric_difference](https://docs.python.org/3/builtins/stdtypes.html#set.symmetric_difference)：返回只出现在一边的元素。
