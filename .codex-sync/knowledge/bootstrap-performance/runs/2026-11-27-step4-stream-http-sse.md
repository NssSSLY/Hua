# 2026-11-27 应用路线第 4 步验收：`std.stream`、`std.url`/`std.http`（HTTPS/WinHTTP）与纯 Hua `std.sse`

## 1. 任务与基线身份

- **任务**：按 4A → 4B → 4C 完成应用驱动五步路线第 4 步（统一流式 `Reader`/`Writer`、`std.url` + 平台无关 `HttpBackend`/Windows `WinHttpBackend`、纯 Hua SDK 标准源码模块 `std.sse`、HUAB 31 与 `huaBin/` 重建，以及 HTTPS/SSE → SQLite Agent 端到端四路径验收）。
- **代码基线**：`main@c72b40e`（未提交/未推送 Git）。
- **平台与工具链**：Windows x64，`llvm-mingw-20260922-ucrt-x86_64`（Clang 23.1.2 / lld），系统内置 `System32\winhttp.dll` + `crypt32.dll`（零三方 DLL）。

## 2. 主要实现改动

### 4A · `std.stream`（14 函数，2 句柄）
- `include/hua/stream.hpp`、`src/stream_stdlib.cpp`：实现 `std.stream.Reader` / `std.stream.Writer` 资源控制块（`Open → Closed / Poisoned`、幂等关闭、退出析构兜底），提供 `from_bytes`/`from_file`/`to_buffer`/`to_file`/`limit`/`read`/`read_exact`/`read_all`/`write`/`write_all`/`flush`/`close_reader`/`close_writer`/`copy`。
- `include/hua/value.hpp`、`src/value.cpp`、`src/sema.cpp`、`src/task_engines.cpp`、`src/native.cpp`、`selfhost/types.hua`：新增 4 个 16 字节 `Value` 分支（`sizeof(Value)` 仍为 48 字节），拒绝 `clone`（`E3008`）与 `spawn`/Task 快照（`E3008`/`E4104`）捕获 `Reader`/`Writer`/`StreamResponse`（`std.http.Response` 保持不可变可跨 Task 传递）。

### 4B · `std.url`（4 函数）与 `std.http`（14 函数，2 类型）
- `include/hua/http_backend.hpp`、`src/url_stdlib.cpp`、`src/http_winhttp.cpp`、`src/http_stdlib.cpp`：
  - `std.url`：`parse`/`encode`/`decode`/`build_query`（严格拒绝控制字符与 userinfo）。
  - `HttpBackend` 抽象与 `WinHttpBackend`：动态加载 `System32\winhttp.dll`，`WINHTTP_FLAG_ASYNC` + 回调状态机 + 20ms 轮询 `RuntimeContext::cancellation` 与 `deadline`；取消/超时时调用 `WinHttpCloseHandle` 并等待 `WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING` 退休后返回；支持手动重定向控制（跨 origin 自动剥离 `Authorization`/`Cookie`/`Proxy-Authorization`、`max_redirects`）、`max_response_bytes`（`body_too_large`）、HTTP 4xx/5xx 视为有效 `ok(Response)`、默认严格 TLS 校验及 `ca_cert_pem`（`crypt32` 内存临时 `HCERTSTORE` + `CertGetCertificateChain` + `CERT_CHAIN_POLICY_SSL` 严格校验）。

### 4C · 纯 Hua `std.sse`、HUAB 31 与双端同步
- `std/sse.hua`：纯 Hua SSE 增量解析器（基于 `std.stream.Reader`，处理 `\n`/`\r\n`/`\r`、任意字节与多字节 UTF-8 跨 chunk 拆包、`event:`/`data:`/`id:`/`retry:`、注释忽略与提前 `close`）；经 `scripts/package-windows-sdk.py` 装配为 `huaBin/src/std/sse.hua`，并在宿主 `src/module.cpp` 与自举 `selfhost/modules.hua` 统一按 `manifest.json` `source_modules` 加载。
- `src/archive.cpp`、`selfhost/{archive,bytecode_verify,lowering,native_cpp,types}.hua`、`include/hua/native_rt.hpp`（补齐 `OkV<std::nullopt_t>` 转换）、`selfhost/stdlib_manifest.json`、`tests/tools/sync_stdlib_contract.py`：升级 `HUAB format 31`（reader 支持 `1–31`），30 内建模块 / 406 函数（自举 `331/406`、`--release` `322/406`、`21/30` 模块四路径全通）+ 2 纯 Hua SDK 模块。

## 3. 代际不动点与 `huaBin/` 重装配实测

- `G1 == G2` PE SHA-256：`a978266e79a7080a4b7583e1b033ef3d8c3630f56b902eec7b23da48cff2824b`
- `G1 == G2` 生成 C++ SHA-256：`e8339e3cc782f3c5335dbcbc684a14b95393e948d79c82fa5ba7479d426b8d42`
- `C2`（`17.335s`）`==` `C3`（`135.648s`，`752,467,173` 步，`peak_calls = 53`）HUAB SHA-256：`74ae55d6f38e6640a8265c60dbfd1849e15b70a34ac3e7ae037b8f8a8cca8abd`（`5,773,061` 字节，`86,067` 指令，`format 17`）
- 补修 `include/hua/native_rt.hpp` `OkV<std::nullopt_t>` 后以 `--finalize-only` 重装 `huaBin/` PCH（`155.426s`）并同步。

## 4. 验收与回归结果

| 测试套件 | 结果 | 关键覆盖 |
|---|---|---|
| `python tests/step4_stream_http_sse_tests.py --sdk huaBin` | **PASS** | 4A+4B（`std.stream` 短读/EOF/限额/拷贝、`std.url`、`std.http` GET/POST/404/同源与跨源重定向剥离 `Authorization`/循环重定向/`body_too_large`/超时/HTTPS 默认拒自签与 `ca_cert_pem` 严格校验）全 4 路径；4C（HTTPS+随机 1–7 字节拆包+多字节 UTF-8 SSE → `packages/sqlite` 事务落库 + 提前关闭）全 4 路径含干净目录独立 EXE；Path 1/2/3 异步 `task.timeout`/取消中断在途 HTTP 与 HTTPS SSE 流；负例 `clone(Reader)` 报 `E3008` |
| `python tests/sqlite_package_tests.py --sdk huaBin` | **PASS** | Step 3C 四路径 + N3 缓存恢复 + 缺 DLL `E7001` 回归全过 |
| `ctest --test-dir tests/build -R "stdlib_contract|archives_extensions|module_contract"` | **PASS** | `stdlib_contract`（`0.56s`，含 4 项负向防漏自检）、`archives_extensions`、`module_contract` 全过 |

## 5. 边界与下一步

- **明确边界**：`hua build --release` 的 `selfhost/native_cpp.hua` 尚未支持 `async fn`/`spawn`（异步取消目前覆盖 `hua run`、宿主 HUAB 31、自举 HUAB 31 三路径）；非 Windows 平台 `HttpBackend` 留为显式 `unsupported` 桩供后续 Linux/iOS 对接。
- **下一步**：应用五步路线进入**第 5 步：由实际应用驱动扩库**（CSV/正则/压缩/流式子进程/Python Bridge 装配，或按需推进原生 `--release` 异步与跨平台）。
