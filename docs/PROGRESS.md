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
| W2 自研件② | Neuron 驱动插件（薄 C 描述符 + C++ 逻辑 + SDK 垫片 + 「假 Neuron」自检） | ✅ 实测（本机垫片路线） |
| W2 传输层 | 真实 MQTT（Paho C 同步 API）+ 断网续传跑在真网络上 + 批量打包 | ✅ 实测 |
| D3 Quick Start | 整条链路 REST 化脚本 | ⏳ 脚本已就绪，**未在 WSL 实跑** |
| D7 | 建仓 + 首次提交 + 打里程碑 tag | ✅ 完成（`3e40760`，tag `v0.1.0`，63 文件 / 9846 行） |

## 二、可直接引用的实测数字

### 2.1 工程质量

| 项 | 数字 |
| -- | -- |
| 单元测试 | **88 用例 / 88 通过 / 0 失败（21,128 断言）** |
| 双编译器 | g++ 16.2.0 与 clang++(LLVM-MinGW) **断言数完全相同** |
| ctest | **8 个用例、全过、约 19 秒** |
| 编译告警 | `-Wall -Wextra -Wpedantic -Wshadow -Werror` **零告警** |
| ASan/UBSan | 核心逻辑**零告警**（WSL，`wsl-asan` preset） |

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

### 2.4 性能（含一条"坏消息"，但它是本项目最值钱的数字）

| 载荷 | 单条发布耗时 | 点位吞吐 |
| -- | -- | -- |
| 1 点位/消息 | 116 ms | **8.9 点位/秒** |
| 50 点位/消息 | ~200 ms | **250 点位/秒**（提升 **27.9×**） |
| 100 点位/消息 | 471 ms | 212 点位/秒（反而降） |

⇒ 打包是必需的，但**同步 API 天花板约 250 点位/秒**；§7 矩阵要的 5000 点位/秒差 **~20×**。
见风险 R9（出路：换 Async API，或据实下调指标）。

## 三、风险表更新

| 风险 | 原判 | 现状 |
| -- | -- | -- |
| R1 插件 SDK 卡壳 | 最大风险，有降级线 | **基本解除**：客户端层已独立验证（20/20）；插件本体用「假 Neuron」实测通过（22/22，对真设备 30/30）。剩余仅"真 SDK 构建与真 Neuron 联调"（需 WSL） |
| R6 构建口径（`-O0` / `CFLAGS` 失效） | 会让性能数据作废 | **已解决并写进文档**：`DISABLE_WERROR` 会让 `$ENV{CFLAGS}` 整段被跳过，必须用 `-DCMAKE_C_FLAGS`；构建后 `compile_commands.json` 自证 |
| R8 libxml2 静态库丢依赖 | 未预见 | **已解决**：新增条目，链接期缺 `-lz -llzma`，成因是裸名链接 + 静态库 |
| R9 MQTT 速率被客户端库限制 | 未预见 | **已量化**：见 2.4。待决策：Async API 改造 or 下调指标 |

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
| 真 SDK 构建插件 + 真 Neuron 联调 | 需 WSL（`wsl.exe` 被安全策略拦） | 按 `plugins/README.md` 的命令跑 |
| Quick Start 闭环（`setup_quickstart_wsl.sh`） | 同上，脚本已就绪未跑 | 一条命令 |
| `ngwp-sim.json` / `plugins.json` 登记 | 属真 SDK 侧 | 与上一条一起 |
| TSan 零竞态 | 目前是单线程，跑了没意义 | 边缘代理上多线程后再跑 |
| Async API 改造 | R9 待决策 | 若冲 100ms 周期大规模档位 |
| 压测报告（图表） | 依赖压测矩阵跑完 | W5 |

## 六、仓库状态

| 项 | 值 |
| -- | -- |
| 首次提交 | `3e40760` — 63 文件 / 9846 行插入 |
| 提交链 | `daf0e03` ← `16e85a6` ← `5ea9590` ← `c5891c9` ← `3e40760`（tag `v0.1.0`） |
| 最新提交 | `daf0e03` — 维护体检后清理：`parser.h` 死声明删除、`.gitignore` 补 `__pycache__`、README 进度口径对齐（**双编译器复验全绿后提交**，21,128 断言与清理前逐位一致） |
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
