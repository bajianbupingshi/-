# W1–W2 复盘（D7）

> 口径约定：**已实测 / 未实测 / 未开始** 三态必须分清。
> 本页所有数字都来自本机或 WSL 的实跑输出，不是估算；每条都能用文末命令复现。

## 一、完成了什么

| 周 | 计划内容 | 状态 |
| -- | -- | -- |
| W1 D1–D3 | Neuron 底座（依赖 + 源码构建 + 面板 + 冒烟） | ✅ 实测 |
| W1 D4–D6 | 工程骨架 + 协议一页定稿 + 协议层（CRC / 编解码 / 状态机）+ 寄存器表与注入 + 表驱动单测 | ✅ 实测 |
| W2 自研件① | 协议设备模拟器（asio 多客户端 + 进程内端到端自检） | ✅ 实测 |
| W2 自研件③ | 边缘数据代理可靠性内核（续传状态机 + 对账 + 反转断言） | ✅ 实测 |
| W2 自研件② | Neuron 驱动插件（薄 C 描述符 + C++ 逻辑 + SDK 垫片 + 「假 Neuron」自检） | ✅ 实测（垫片路线 + 真 SDK 构建 + **真 Neuron 建节点跑通**，2026-10-06） |
| W2 传输层 | 真实 MQTT（Paho C 同步 API）+ 断网续传跑在真网络上 + 批量打包 | ✅ 实测 |
| D3 Quick Start | 整条链路 REST 化脚本 | ✅ 实测（2026-10-06 真环境跑通：南向采值、北向 MQTT 每秒一条、订阅 `#` 收到报文） |
| D7 | 建仓 + 首次提交 + 打里程碑 tag | ✅ 完成（`3e40760`，tag `v0.1.0`，63 文件 / 9846 行） |
| W3-1 | SQLite 环形缓存：IRecordStore 接口抽取 + 持久化实现（WAL + seq 水位线）+ 重启续传验收 | ✅ 实测（2026-10-07：WSL 93/93 用例含 SqliteStore 5 项，验收场景跨重启对账丢失 0/重复 0；Windows 88/88 垫片路径无回归） |
| W3-2 | 多线程服务壳：BoundedQueue + EdgeProxyService（采集→管道 actor 模型，内核单线程独占） | ✅ 实测（2026-10-07：**TSan 101/101 零竞态**，WSL Release 101/101、Windows 96/96；CI 新增 tsan job） |
| W3-3 | 规则引擎：阈值（迟滞+冷却）/ 变化率 → 告警；JSON 配置加载；采样观测钩子挂进管道线程 | ✅ 实测（2026-10-07：表驱动 + **确定性重放**（双引擎逐字段比对）+ 服务集成；WSL 107/107、TSan 零警告、Windows 102/102） |
| W4-1 | 告警发布链路：gw_alerts（独立 MQTTAsync 客户端 + JSON 主题）+ mini broker 路由修复 + Paho 升 1.3.14 | ✅ 实测（2026-10-07：真 TCP/MQTT 端到端收告警并逐字段断言；WSL 108/108、TSan 零警告、Windows 103/103） |

## 二、可直接引用的实测数字

### 2.1 工程质量

| 项 | 数字 |
| -- | -- |
| 单元测试 | **103 用例（Windows 垫片）/ 108 用例（WSL 真框架，含 SqliteStore 5 + BoundedQueue 4 + ProxyService 4 + Rules 6 + AlertPublish 1）** 全通过；**TSan 零竞态**（wsl-tsan，107/107） |
| 双编译器 | g++ 16.2.0 与 clang++(LLVM-MinGW) **断言数完全相同** |
| ctest | **8 个用例、全过、约 19 秒** |
| 编译告警 | `-Wall -Wextra -Wpedantic -Wshadow -Werror` **零告警** |
| ASan/UBSan | 核心逻辑**零告警**（WSL，`wsl-asan` preset） |
| WSL 三套 preset（2026-10-06 实测） | `wsl-debug` / `wsl-release` / `wsl-asan`：ctest **8/8**、**88/88** 单测（系统 GoogleTest，12 套件）、演示 18/18，三套确定性哈希一致 |

### 2.1.1 ★ WSL 复验抓到的真 bug（双平台互检第二例）

2026-10-06 首次在 WSL 跑齐三套 preset（此前 WSL 记录停留在 52 用例时代，且 preset 的
`GW_BUILD_DRIVER=ON` 是 D7 才补的，从未被 Linux 实测过）：

```
libgw_client.a(client.cpp.o): relocation R_X86_64_TPOFF32 against symbol
asio::... can not be used when making a shared object; recompile with -fPIC
```

- **根因**：`gw_driver` 是共享库，它链接的静态库 `gw_client` / `gw_common` 默认不带 `-fPIC`；
- **为什么本机没暴露**：Windows/MinGW（PE）没有这类重定位限制，双编译器也编不出来 ——
  只有目标类平台（Linux）能暴露。与「clang 抓 `-Wunused-private-field`」同类：
  **互检矩阵多一维，就能多抓一类问题**；
- **修法**：根 `CMakeLists` 全局 `set(CMAKE_POSITION_INDEPENDENT_CODE ON)`（`df21fd4`），
  Windows 侧重跑 8/8 无回归；
- **教训**：CI 里那两条 wsl preset（也开 DRIVER）在修复前一旦推送**必挂** —— 复验跑在推送之前，值。

### 2.1.2 ★ 真 Neuron 联调跑通（R1 完全解除，2026-10-06）

真 SDK 头构建 → dlopen 冒烟（22/22）→ 完整链路（30/30）→ 漂移复核（528 常量一致）
→ **真 Neuron 加载插件 + REST 建节点全流程 error 0 + 连续读回正弦活数据**。
复现：`tools/ngwp_node_rest.sh`。垫片与真头的 4 类形状差异（全部已回写源码与垫片）：

| 差异 | 症状 | 修法 |
| -- | -- | -- |
| 回调藏在 `union{struct driver}` | `->update` 编不过 / 节点回调静默失效 | 访问改 `->driver.update`，垫片补同形嵌套 |
| `UT_array` 是 uthash 的 `d/icd.sz/i/n` | 手工构造数组的测试代码两路漂移 | 垫片逐字对齐 uthash 2.3.0 |
| `neu_dvalue_t` 是 `struct{type,value,precision}` | 真 Neuron 按 `type` 解释载荷，不赋值=未定义行为 | 垫片对齐 + `dv.type=NEU_TYPE_UINT16` |
| `common.magic` 必须是 `0x43474D50`（"PMGC"） | `neu_adapter_create` 的 assert **打崩整个 Neuron 进程** | open() 里赋值（`#ifndef` 兜底宏，.so 零外部符号依赖） |

另有 6 处常量漂移（NODE/GROUP_NAME_LEN=128、TAG_META=20、TAG_FORMAT=16、
TYPE_ERROR=15、VERSION_MINOR=15）由 `tools/check_sdk_shim.py` 抓出后对齐。
外围坑：gw_client 原挂在 `GW_BUILD_SIM` 之下（单独开 DRIVER 时静默降级 `-lgw_client`）、
真 SDK 的 json 封装要补 `<SDK>/neuron` 这条 include、本 daily 版 `/api/v2/ping` 只认 POST。

### 2.2 确定性（这是我认为最硬的一条）

同一个 seed 的字节流哈希 = **`0xDC11C55788C2B000`**，在以下五种组合下**完全一致**：
g++ / clang++、Windows(MinGW) / Linux(GCC 13)、`-O0` / `-O2`。
换 seed 则变（`0xFF824424A6CFCBD6`）—— 这条是**反转断言**，防止"哈希其实是个常数"的假绿。

### 2.3 可靠性（简历头条那条）

| 场景 | 实测 |
| -- | -- |
| 断网 10 分钟（30 分钟时长、250ms 周期、7200 点） | **丢失 0 / 重复 0**，积压峰值 **2406 条**（≈10min÷250ms），重连 1 次 |
| 真 MQTT 上断网（关掉 broker 再恢复） | **丢失 0 / 重复 0**，积压峰值 175 条消息（1600 点位） |
| 反转：静默丢包（每 97 次丢 1） | 对账器报出 **74 条丢失**（7200÷97≈74.2 ✓） |
| 反转：容量打满 | 报出 **2306 条**，与 `store.dropped()` **精确相等** |
| 反转：TTL 过短 | 报出 **2166 条**，与 `store.expired()` **精确相等** |

### 2.4 性能（两幕剧：先发现天花板，再拆掉它）

**第一幕（2026-10-06，同步 API 时代的"坏消息"）**：

| 载荷 | 单条发布耗时 | 点位吞吐 |
| -- | -- | -- |
| 1 点位/消息 | 116 ms | **8.9 点位/秒** |
| 50 点位/消息 | ~200 ms | **250 点位/秒**（提升 **27.9×**） |
| 100 点位/消息 | 471 ms | 212 点位/秒（反而降） |

⇒ 当时结论：同步 API 天花板约 250 点位/秒，§7 矩阵要的 5000 点位/秒差 ~20×。

**第二幕（2026-10-07，R9 改造完成）**：读 Paho C 1.3.13 源码实锤——
`MQTTAsync_waitForCompletion` 内部就是 `sleep(100ms)` 轮询（同步/异步共用），
这才是 116ms 地板的真凶。改为 `onSuccess/onFailure` 回调 + 条件变量的事件驱动等待
（`ITransport` 的「Ok = PUBACK 已确认」语义零变化）后：

| 口径 | 同步时代 | Async + 事件驱动（WSL Release / Win Debug） | 提升 |
| -- | -- | -- | -- |
| 1 点位/消息 | 8.9 点位/秒 | **6126 / 1490 点位/秒** | **688× / 167×** |
| 30 点位/消息 | — | **134 421 / 59 826 点位/秒** | — |
| 100 点位/消息 | 212 点位/秒 | **357 633 点位/秒**（WSL Release） | **1687×** |

⇒ §7 矩阵的 5000 点位/秒**被超越 27×**（30 点位/批），「据实下调指标」不再需要；
打包从「绕地板的唯一手段」降级为常规优化（A/B 仍有 22~58× 差）。
断网续传自检 18/18 原样通过，selftest 耗时 27s → ~1.4s。详见 README 的 R9 改造小节。

## 三、风险表更新

| 风险 | 原判 | 现状 |
| -- | -- | -- |
| R1 插件 SDK 卡壳 | 最大风险，有降级线 | **完全解除**（2026-10-06）：真 SDK 头构建通过（漂移复核 528 常量一致）、真 Neuron 加载建节点跑通（连续读回活数据）。垫片与真头的 4 类形状差异已回写源码（见 2.1.2） |
| R6 构建口径（`-O0` / `CFLAGS` 失效） | 会让性能数据作废 | **已解决并写进文档**：`DISABLE_WERROR` 会让 `$ENV{CFLAGS}` 整段被跳过，必须用 `-DCMAKE_C_FLAGS`；构建后 `compile_commands.json` 自证 |
| R8 libxml2 静态库丢依赖 | 未预见 | **已解决**：新增条目，链接期缺 `-lz -llzma`，成因是裸名链接 + 静态库 |
| R10 静态库链接进共享库缺 PIC（WSL 复验抓到） | 未预见 | **已解决**（`df21fd4`）：全局 `CMAKE_POSITION_INDEPENDENT_CODE ON`；Windows/MinGW 不报此错，只有 Linux 暴露 —— 见 2.1.1 |
| R9 MQTT 速率被客户端库限制 | 未预见 | **已解决**（2026-10-07）：Async API + 事件驱动等 PUBACK，天花板 250 → 13.4 万点位/秒；根因是 Paho waitForCompletion 的 100ms 轮询（源码实锤），见 2.4 |

## 四、面试可讲清单（每条都有实物，不是背概念）

| 讲点 | 实物在哪 |
| -- | -- |
| TCP 粘包/拆包 + 显式状态机 | `common/src/parser.cpp`；`std::variant` 阶段 + `std::visit`；逐字节喂入与一次性喂入事件序列一致（单测强制） |
| 重同步策略是个取舍，没有免费答案 | `ResyncPolicy` 两档；实测同一坏帧：1 个错误事件 vs 13 个，都不漏帧 |
| 两份独立实现互证 | CRC 位运算实现 + 256 项查表实现逐位比对，并对齐 CRC-16/MODBUS 标准值 `0x4B37` |
| **C++ 对象跨动态库 ABI 边界** | `plugins/driver/plugin_module.c`（薄 C 描述符）+ `driver_impl.cpp`（C++ 逻辑）；**实测**：`.driver = {…}` 嵌套指定初始化器 C99 合法、**C++20 禁止**，纯 C++ 编不过 |
| **为什么异常不能穿越 `extern "C"`** | 每个导出函数 `try/catch(...)` 全包；自检里 `group_timer(NULL)/setting(NULL)` 必须返回错误码而不崩 |
| 对账必须有两个口径 | `missing_within_received()` vs `missing_against_produced()`；**踩过**：只看前者会把"从未发出的数据"当成没丢 |
| 断网续传状态机 | `LinkState{ Live, Backfill, CatchingUp }`，状态由「上次发送结果 + 积压是否为空」推导而非外部设置 |
| 确定性重放怎么做才可信 | 自定义 splitmix64（不用 `<random>`，其分布实现跨标准库不保证一致）；跨 3 工具链 × 2 平台 × 2 优化级别验哈希 |
| 抗内存放大 | 先校验 `LEN ≤ 261` 再分配缓冲 |
| 每个结论都配反转断言 | 确定性→换 seed 必须不同；可靠性→故意丢包必须报出丢失且数量吻合 |

## 五、未做 / 待做

| 项 | 原因 | 下一步 |
| -- | -- | -- |
| ~~TSan 零竞态~~ | ✅ **已达成**（2026-10-07）：EdgeProxyService 上线后 wsl-tsan 101/101 零警告；附赠 TSan 实战经验（测试装置竞态 / 新内核 ASLR 崩溃 / GCC13 -Wtsan），见 README W3-2 节 |
| ~~规则引擎~~ | ✅ **已完成**（2026-10-07）：gw_rules target（nlohmann/json 挡在 .cpp）+ 阈值迟滞/冷却状态机 + 变化率斜率判定 + 确定性重放单测；告警的 MQTT 发布与面板展示归 W4 |
| 轻量面板（cpp-httplib + ECharts） | W4-2 未开始；告警 JSON 已就绪可直接消费 | 面板展示实时值/告警 |
| 压测报告（图表） | 依赖压测矩阵跑完 | W5（R9 改造后 100ms 档位已可达，矩阵可全量跑） |

> 已从本表移除（2026-10-06 完成）：真 SDK 构建插件 + 真 Neuron 联调、
> `ngwp-sim.json` 登记 —— 见 2.1.2 与 `tools/ngwp_node_rest.sh`。

## 六、仓库状态

| 项 | 值 |
| -- | -- |
| 首次提交 | `3e40760` — 63 文件 / 9846 行插入 |
| 提交链 | `961a696` ← `752a775` ← `f0f1775` ← `e15ae30` ← `a0ae824` ← `65cf1f6` ← `10e42ea` ← `df21fd4` ← `daf0e03` ← `16e85a6` ← `5ea9590` ← `c5891c9` ← `3e40760`（tag `v0.1.0`） |
| 最新提交 | `961a696` — feat: W3-1 SQLite 环形缓存（IRecordStore 接口抽取 + 持久化实现 + 水位线 + 重启续传验收全绿）。此前：`752a775` R9 Async 改造（天花板 250 → 13.4 万点位/秒）、`e15ae30` 真 Neuron 建节点联调（R1 完全解除） |
| 里程碑标签 | `v0.1.0`（annotated，指向 `3e40760`） |
| 分支 | `main` |
| 仓库体积 | `.git` 约 370 KB（**无构建产物**：`build*` / `*.o` / `*.log` / `CMakeCache` / `__pycache__` 全部被忽略） |
| 换行 | `.gitattributes` 强制全文 LF —— 否则签出的 `.sh` 在 WSL 上会 `bad interpreter` |
| 代码构成 | 23 `.cpp` / 13 `.h` / 10 `CMakeLists.txt` / 7 `.sh` / 5 `.md` / 1 `.yml` / 1 `.json` |

> 2026-10-06 维护体检记录：全面复验（双编译器 `-Werror` 零告警、88/88 单测、ctest 8/8
> —— 补跑了 `GW_BUILD_DRIVER=ON` 配置、插件完整链路 30/30、脚本/CI/HTML 结构核对）全绿后，
> 才提交上述清理；`tests/gtest_shim.h` 及全部测试源码未动。

推送：

```bash
git remote add origin <你的仓库地址>
git push -u origin main --tags
```

> 首次提交的 identity 是通过 `tools/first_commit.sh` 以**仓库级**配置写入的（不动全局）。
> 原因：本机全局 git 身份是 `Migration Agent <automation@example.com>`（自动化占位值），
> 用它提交会污染 GitHub 的贡献者图。

## 七、复现命令

```bash
# 本机（Windows）：双编译器 + 全部自检（含模拟器/MQTT/客户端/插件）
bash gateway/tools/build_host.sh

# 本机 ctest（8 个用例）
cd gateway && cmake -B build -G Ninja -DGW_WITH_MQTT=ON -DGW_BUILD_SIM=ON -DGW_BUILD_DRIVER=ON \
  -DGW_PAHO_INCLUDE_DIR=<paho>/include -DGW_PAHO_LIBRARY=<paho>/lib/libpaho-mqtt3c-static.a
cmake --build build -j && cd build && ctest

# 插件完整链路（真设备 → 客户端 → 插件 → 回调）
gw_sim --port 15020 --interval 600000 &
driver_check --lib <插件库> --device-port 15020

# 性能对比
mqtt_e2e --bench --count 200 --points 50

# WSL：三套构建 + 测试
bash gateway/tools/bootstrap_wsl.sh
```
