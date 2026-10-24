# HUA-D038第5步第四组接口提案：fs walk/copy（2026-10-24用户定稿）

状态：**2026-10-24用户定稿**，W1–W7全部按推荐（W1目录以`/`结尾的单个`[]string`、W2 copy_file不覆盖、W3 copy_dir遇链接FS_TYPE停止、W4提供remove_all并拒绝根与启动cwd、W5遍历出错立即Err、W6纳入canonical、W7 HUAB17）；尚未实施。依据：[标准库设计与实现路线](../../../../docs/标准库设计与实现路线.md)对齐清单std.fs/path/io行（P1 `walk`、`copy`、`canonical`；P2 `glob`、权限细节）；现有合同`docs/标准库与内建接口.md`“std.fs”“S1a：路径与文件状态详细合同”（路径按启动cwd解析、不跟随末项链接、`PATH_*`/`FS_*`错误前缀、Task中阻塞适配与取消检查）；实现`src/stdlib.cpp`（旧fs）、`src/filesystem_stdlib.cpp`（S1a）。格式沿用[os提案](2026-10-24-os-exit-platform-proposal.md)。

## 0. 现状事实（影响设计）

| 事实 | 位置 | 影响 |
| --- | --- | --- |
| 已有`list_dir`（一层、按UTF-8字节排序、只返回名称）、`stat`（不跟随末项链接，返回Json）、`read_bytes/write_bytes`（≤128 MiB）、`mkdir_all`、`remove`（不递归） | 接口文档std.fs | 递归遍历今天只能“list_dir＋逐项stat”手写（每项一次标准调用＋一次Json构造）；复制只能整文件读入再写出 |
| S1a合同：路径须非空、无NUL、有效UTF-8、≤32767字节；按启动cwd补成绝对路径；Windows拒绝设备名/非法字符/不完整根；错误为带前缀的string | S1a合同 | 新接口沿用同一路径校验与错误前缀，新增`FS_LIMIT` |
| std.fs在Task中按`suspends_in_task`走阻塞适配，操作前检查取消 | src/runtime.cpp、S1a合同 | 递归操作在循环中每处理若干项检查取消，宿主阻塞调用本身仍不可中断 |
| 没有Tuple；有多返回 | 语言设计 | 遍历结果的“种类”需要编码进结果或多返回（见W1） |
| 标准Lua没有目录接口 | — | 基准只对照CPython |

## 1. 函数（`since` 17）

| 函数 | 签名 | 语义 |
| --- | --- | --- |
| `walk` | `walk(root string) Result<[]string,string>` | 递归列出root下的全部项（不含root本身），返回相对root的路径，分隔符`/`；**目录以`/`结尾**（W1）；先序深度优先，每层按UTF-8字节排序（与`list_dir`一致），结果确定 |
| `copy_file` | `copy_file(source string, target string) Result<bool,string>` | 复制普通文件内容；目标已存在为`FS_EXISTS`（不覆盖，W2）；源为目录/末项链接/其他为`FS_TYPE`；目标父目录须存在 |
| `copy_dir` | `copy_dir(source string, target string) Result<int,string>` | 递归复制目录树（目录与普通文件），返回复制的文件数；target须不存在（`FS_EXISTS`）；遇到链接或其他特殊项为`FS_TYPE`并停止（W3）；失败时已复制部分保留（不回滚） |
| `remove_all` | `remove_all(path string) Result<int,string>` | 递归删除，返回删除的项数；不存在为Ok(0)；不跟随链接（只删链接本身）；**拒绝文件系统根与启动cwd本身**（`FS_TYPE`，W4） |
| `canonical` | `canonical(path string) Result<string,string>` | 解析`.`/`..`与全部符号链接得到绝对路径（`/`分隔）；路径须存在（`FS_NOT_FOUND`） |

- 遍历规则：**不跟随符号链接**（链接项按非目录列出，不进入，避免环）；只列出普通文件、目录与链接，其他特殊项按非目录列出。
- 上限：`walk`最多1,000,000项、目录深度256，超出为Err `FS_LIMIT`（不产生部分结果）；`copy_dir`/`remove_all`同样受这两项上限。
- 失败语义（W5）：遇到无法读取的子目录立即返回Err（带出错路径），不跳过、不返回部分列表。
- Task：沿用fs的阻塞适配；遍历/复制/删除每处理256项检查一次取消（E4101诊断）。
- 复制只承诺内容与可执行位以外的普通数据；时间戳/权限/ACL按宿主`copy_file`默认行为，不承诺保留（文档写明）。

## 2. 版本与实现位置

- 新函数`since` 17→HUAB17；不新增类型、不新增opcode。
- 实现：`src/filesystem_stdlib.cpp`（复用S1a的路径校验与错误格式化），宿主调用用`std::filesystem`（`directory_iterator`、`copy_file(copy_options::none)`、`symlink_status`、`canonical`），Windows另用`CopyFileExW(COPY_FILE_FAIL_IF_EXISTS)`保证“不覆盖”由系统原子判定。
- 不改selfhost（自举暂停，L0-4登记）。

## 3. 基准（实施前先测手写基线；`perf/stdlib/fs`，参照只有CPython）

- fs/01 walk_manual：基准程序先在临时目录建20×20个目录、每个10个文件，然后“list_dir＋stat判种类”递归20轮 → native `walk`；参照Python `os.walk`。
- fs/02 copy_manual：200个4 KiB文件，`read_bytes`＋`write_bytes`逐个复制 → native `copy_file`；再对整棵树`copy_dir`，参照`shutil.copyfile`/`shutil.copytree`。
- 两个基准都在结束时清理临时目录（手写版用逐项`remove`，原生版用`remove_all`），输出只含计数，保证三种语言一致。

## 4. 验收

1. 三路径一致：walk的顺序/目录后缀/空目录/深层嵌套/Unicode名称；链接不进入（本机能否创建符号链接视权限，不能创建时如实跳过并标注）；copy_file的内容逐字节一致、目标已存在拒绝、源为目录拒绝；copy_dir的文件数与树结构一致、target存在拒绝；remove_all计数、不存在Ok(0)、拒绝根与cwd；canonical对`..`与相对路径。
2. 上限：深度257、超项数的`FS_LIMIT`（用小上限的测试钩子或深度构造）；路径校验沿用S1a的`PATH_INVALID`。
3. Task中调用与取消（E4101）。
4. HUAB17往返、旧程序格式不变、降级头E6002；`er0_inventory`新增ER0-AB行。
5. 基准表写入路线文档；CTest全套（archives_extensions为HUA-D037已知）；自举C2逐字节不变；Linux/macOS未在本机验证，如实标注。

## 5. 需要你决定的点

- **W1 walk结果形态**：推荐单个`[]string`、目录以`/`结尾（一次调用就能区分种类，无需再stat）；备选多返回`([]string, []string)`（路径与种类对齐），或只返回文件。
- **W2 copy_file覆盖**：推荐不覆盖（`FS_EXISTS`，与`rename`一致，由系统原子判定）；备选覆盖，或另加`overwrite`布尔参数。
- **W3 copy_dir遇到链接**：推荐报`FS_TYPE`并停止（不猜用户意图）；备选复制链接本身，或跟随链接复制目标内容。
- **W4 remove_all**：推荐提供并拒绝文件系统根与启动cwd（递归删除是常见需求，但须有护栏）；备选本组不提供递归删除。
- **W5 遍历出错**：推荐立即Err（无部分结果）；备选跳过不可读子目录并在结果外另报。
- **W6 canonical**：推荐纳入本组（P1清单项，实现简单）；备选后置。
- **W7 HUAB17**：推荐新格式号17。

## 6. 实施记录（2026-10-24）

- 全部按定稿实现（fs新增5个，HUAB17）。比提案更严/更稳的三处：copy_dir拒绝target在source内；remove_all也拒绝启动目录的上级；Windows用`\\?\`长路径形式（实测超过260字符会失败）。
- 复制实现：`CopyFileExW`比手写读写慢（0.420/0.323秒），改为排他创建＋复用64 KiB缓冲流式复制（0.312秒）；不复制时间戳/属性。
- 结果表见docs/标准库设计与实现路线.md“第5步第四组fs walk/copy结果”。
