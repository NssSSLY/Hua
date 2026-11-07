# HUA-D038 P1：checked/wrapping/saturating整数与双曲函数

状态：**2026-11-04用户明确确认“按推荐方案实施24个”**。本批新增函数已实现，since22/HUAB22；实际结果见[运行记录](../runs/2026-11-04-p1-math-arithmetic.md)。

## 定稿接口

对add/sub/mul/neg/abs/pow各提供checked_、wrapping_、saturating_三种前缀，共18个函数。add/sub/mul/pow参数为两个默认int，neg/abs为一个默认int。

| 家族 | 返回类型 | 规则 |
|---|---|---|
| checked_* | Result<int,string> | 可表示时Ok；溢出Err，错误前缀MATH_OVERFLOW |
| wrapping_* | int | 数学结果按模2^64保留低64位，再按有符号补码解释 |
| saturating_* | int | 数学结果钳制在[-2^63,2^63-1] |

全部pow指数必须非负；0**0=1。checked_pow的负指数为Err(MATH_DOMAIN: exponent must be non-negative)，wrapping/saturating_pow负指数为E4003；不把巨大指数做成线性循环。wrapping_abs/neg(min_int)=min_int，saturating_abs/neg(min_int)=max_int；checked版本返回Err。(-2)^63在checked/saturating中须精确返回min_int，不因中间平方误报溢出。

另补sinh/cosh/tanh/asinh/acosh/atanh六函数，参数显式默认float，返回Result<float,string>。acosh要求x>=1，atanh要求-1<x<1，非法域Err MATH_DOMAIN，非有限结果Err MATH_OVERFLOW；有限下溢可为零。sinh/tanh/asinh/atanh保持-0；acosh(1)=+0。平台libm精度不承诺逐位相同。

非默认数值类型/错误类型参数静态E3004、动态E4003；个数不符沿用E3005/E4003。原算术运算符及/真除法、//向下整除、%规则保持。此批不含division/remainder或shift的三种包装，不把Rust截断整除规则引入Hua。

## 实现边界

- 无新语言类型、opcode、ABI、隐式转换或全局整数模式。数学函数仍从既有math默认类型检查进入。
- 新整数/双曲实现独立于旧算法，非幂的64位整数运算在128位临时值中精确计算，再按家族处理；回绕用无符号转换和bit_cast。
- 幂采用至多63个指数位的平方求幂：wrapping在uint64模环，checked/saturating按最终符号的2^63或2^63-1限额提前判断；不创建大整数、不循环指数次。
- 双曲函数用libm专门函数，避免以exp/log小输入公式作为生产实现；沿用Task取消入口。
- HUAB writer按最大since自动选22，reader支持1–22；上一批位工具单独仍写21。L0-4暂停中的Hua前端签名/格式同步扩展为9–22待办。

## 验收与参考

独立Python大整数/模幂和math函数oracle覆盖三路径；C++原生检查类型、个数、取消、非有限输入和负零。性能先冻结since21运行时及手写基线，再测原生版本、同机CPython/Lua和core A/B。原始数据及初始测试夹具错误保留在tests/artifacts/stdlib-p1-arithmetic-20261104。

参考[Rust i64家族](https://doc.rust-lang.org/std/primitive.i64.html#method.checked_add)的可恢复溢出/回绕/饱和区分；Hua checked返回已确认的Result而非Rust Option。[双曲函数](https://doc.rust-lang.org/std/primitive.f64.html#method.asinh)仅作函数族参考；错误与有限值合同以Hua现有math为准。
