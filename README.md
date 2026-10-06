# 工业物联网关 · 自研协议层（W1 交付）

> 本目录是《工业物联网关落地方案》的代码落地部分。
> **当前进度：W2 全部完成（D7 已建仓，tag `v0.1.0`）** —— 协议层（CRC16 / 帧编解码 / 显式状态机）+ 设备模拟器（asio）+ 边缘代理可靠性内核 + 真实 MQTT 传输（Paho）+ Neuron 驱动插件（SDK 垫片路线）全部实测；WSL 真环境 Quick Start 链路已跑通。
> **当前进度：W3-2 多线程化完成**（EdgeProxyService 服务壳 + TSan 101/101 零竞态，2026-10-07）；W3-1 环形缓存、R9 Async 改造同日完成（见下文）。
> **尚未开始**：W3-3 规则引擎（阈值/变化率 + JSON 配置）、压测矩阵与实验报告、面板与 QEMU aarch64。

## 目录

```
gateway/
  docs/protocol.md          协议一页定稿（NGWP v1：帧格式 / 字节序 / 功能码 / 异常码 / CRC / 状态机 / 标准向量）
  common/include/gw/        crc16 · frame · parser · register_table · injection · device_model · **reliability**
  common/src/               实现
  apps/proto_demo/          协议层 CLI 演示（编码 → 解析 → 注入 → 确定性重放）
  apps/proxy_demo/          断网续传场景复现器（含三条反转断言）
  apps/sim/                 NGWP 设备模拟器（asio，含进程内端到端自检）
  apps/mqtt_e2e/            真实 MQTT 端到端（Paho + 真 broker + 断网续传 + 接收端对账）
  mqtt/                     真实 MQTT 传输（Paho 隔离在独立 target，gw_common 保持零依赖）
  client/                   NGWP 同步客户端（插件与自检共用的设备对话方）
  store/                    **SQLite 持久化记录库**（W3 环形缓存：IRecordStore 的落盘实现，
                            断电续传 + seq 水位线，GW_BUILD_SQLITE=ON 时构建）
  plugins/driver/           **Neuron 驱动插件**（薄 C 描述符 + C++ 逻辑 + SDK 垫片 + 假 Neuron 自检）
  tests/                    表驱动单测 + GoogleTest 兼容垫片
  tools/build_host.sh       本机双编译器自检（含模拟器与 MQTT 端到端）
  tools/mqtt_real_broker_wsl.sh   对着真 broker（NanoMQ/docker）验证续传并两端对账
  tools/bootstrap_wsl.sh    在 WSL 里一键：投递 → 装依赖 → 三套构建 → 测试 → 演示
  tools/to_wsl.sh           Windows 侧：打印上面那条 WSL 命令（不做投递）
  tools/bootstrap_neuron_wsl.sh   在 WSL 里构建 Neuron 底座（依赖验证 + 两个上游陷阱修复 + 冒烟）
  tools/setup_quickstart_wsl.sh   把「Modbus 模拟器 → Neuron → MQTT」整条链路 REST 化跑通并自证
```

## 实测数据（Windows / MinGW-W64，2026-10-05）

```bash
./tools/build_host.sh
```

| 编译器 / 测试框架 | 构建 | 单元测试 | 演示 / 场景 / 模拟器 / MQTT / 插件 |
| -- | -- | -- | -- |
| g++ 16.2.0 (MinGW-W64 UCRT) + 内置垫片 | 通过（`-Wall -Wextra -Wpedantic -Wshadow -Werror` 零告警） | **88 tests, 88 passed, 0 failed（21,128 assertions）** | 协议演示 18/18；续传场景 6/6（丢失 0）；模拟器 23/23；MQTT 端到端 18/18；客户端 20/20；**插件 ABI 22/22（+完整链路 30/30）** |
| clang++ (LLVM-MinGW) + 内置垫片 | 同上 | **88 tests, 88 passed, 0 failed（21,128 assertions）** | 协议演示 18/18；续传场景 6/6（其余仅编译检查，见下） |
| **真实 GoogleTest 1.12.1**（`GW_FETCH_DEPS=ON` 拉取） | 同上 | 52 → 已扩到 88 用例，未重跑真实框架 | 18 checks, 0 failed |

`ctest` 共 **8 个用例**、全过、约 19 秒。

三点意义：

1. 两套独立编译器给出**完全相同的断言数**，这本身就是确定性的一层证据。
2. 同一份测试源码在**内置垫片**与**真实 GoogleTest** 下结果一致 ⇒ 消除了
   「垫片语义比真实框架宽松、于是假绿」这个风险。
3. **双编译器互检抓到了单编译器漏掉的真问题**：clang++ 报出
   `SimServer::io_` 字段「存了但从未使用」（`-Wunused-private-field`），g++ 不报。
   这就是坚持两个编译器的理由。

### WSL 侧实测（Linux / GCC 13.2，Ubuntu 24.04，真实 GoogleTest 1.14.0）

```
preset: wsl-debug    ctest 100% passed  |  52 tests from 6 test suites  |  [ PASSED ] 52 tests  |  18 checks, 0 failed
preset: wsl-release  ctest 100% passed  |  52 tests from 6 test suites  |  [ PASSED ] 52 tests  |  18 checks, 0 failed
preset: wsl-asan     ctest 100% passed  |  52 tests from 6 test suites  |  [ PASSED ] 52 tests  |  18 checks, 0 failed
```

`wsl-asan` 用 `-fsanitize=address,undefined`，**零告警** —— 没有内存错误，也没有未定义行为。

### 确定性实测证据（同一 seed，三种维度全部一致）

| 维度 | 配置 | `FNV-1a(seed=20261005)` |
| -- | -- | -- |
| 编译器 | g++ 16.2.0（Windows/MinGW） | `0xDC11C55788C2B000` |
| 编译器 | clang++（LLVM-MinGW） | `0xDC11C55788C2B000` |
| 优化级别 | Debug（`-O0`） | `0xDC11C55788C2B000` |
| 优化级别 | Release（`-O2 -DNDEBUG`） | `0xDC11C55788C2B000` |
| 平台 | Linux / GCC 13.2（WSL, `wsl-debug`） | `0xDC11C55788C2B000` |
| 平台 | Linux / GCC 13.2（WSL, `wsl-release`） | `0xDC11C55788C2B000` |
| **反转断言** | 换种子 `seed+1` | `0xFF824424A6CFCBD6`（**必须不同** ✓） |

这条哈希链同时否掉了三类常见失败：**优化级别依赖、编译器依赖、平台依赖**。
最后一行是**反转断言** —— 只有它存在，「哈希一致」才不是「哈希其实是个常数」的假绿。
`bootstrap_wsl.sh` 现在会自动把各 preset 的哈希抽出来并**强制要求全部相同**，不一致即判失败。

演示程序的关键实测输出（`proto_demo`）：

```
[3] 粘包 / 拆包等价性
  字节流 306 B：一次性喂入解出 18 帧 / 0 错误；逐字节喂入解出 18 帧 / 0 错误

[4] 坏帧重同步：翻转 1 个 bit 后的行为
  kSkipFrameOnCrcError : 解出 11 帧，1 个错误
  kScanByByte          : 解出 11 帧，13 个错误

[5] 确定性重放
  字节流长度        : 816 B
  FNV-1a(seed=20261005) : 0xDC11C55788C2B000
  再跑一次              : 0xDC11C55788C2B000  (一致)
  FNV-1a(seed+1)        : 0xFF824424A6CFCBD6  (不同)
```

> 这些数字是**实跑**结果，不是预设值。口径：Debug 构建、`-Werror`、Windows 主机；
> WSL / Release / ASan / TSan 四套构建见 `CMakePresets.json`，尚未在 WSL 上跑过。

## 在 WSL 里构建（本项目正式目标环境）

**一条命令就够**（在 WSL 的 shell 里执行）：

```bash
bash "/mnt/e/桌面项目/agent/嵌入式/4/gateway/tools/bootstrap_wsl.sh"
```

它会：投递到 `~/gateway` → 装依赖（cmake / ninja / g++ / libgtest-dev）→
依次跑 `wsl-debug` / `wsl-release` / `wsl-asan` 三套配置，各跑单元测试与演示自检 →
打印真实通过数与确定性重放证据。`SKIP_APT=1` 可跳过装包，`PRESETS="wsl-debug"` 可只跑一套。

> **为什么不用「Windows 侧投递文件到 WSL 的 /tmp」那套（已废弃）**：
> WSL 的 `/tmp` 是 **tmpfs**，发行版空闲回收（默认约 60s）或重启后内容**全部丢失**。
> 曾经投递完 30 个文件、一分钟后用户进去发现目录不存在 —— 就是这个原因。
> 现在不做任何预投递：工程本来就在 Windows 磁盘上，WSL 通过 `/mnt/e` 直接读得到，
> 由 WSL 侧脚本自己拷进 `~/gateway`。`tools/to_wsl.sh` 现在只负责打印上面那条命令。

`GW_FETCH_DEPS=ON` 时优先用系统 GoogleTest（`libgtest-dev`），找不到才 FetchContent 拉取。

## Neuron REST API 契约（逐个读上游源码确认，不是猜的）

W2/W3 的驱动插件与数据代理都要跟 Neuron 打交道，这里把踩过的字段名固定下来 ——
**凡涉及 Neuron 接口，先查这张表；表里没有的，去读源码，不要按惯例猜。**

| 接口 | 方法 | 请求体 | 备注 |
| -- | -- | -- | -- |
| `/api/v2/ping` | GET | — | 免鉴权探针，返回 `{}` |
| `/api/v2/login` | POST | `{"name":"admin","pass":"0000"}` | **字段是 `pass`，不是 `password`**。写错返回 `{"error":1002}` |
| `/api/v2/node` | POST | `{"plugin":"Modbus TCP","name":"n1"}` | `plugin` 用 `module_name`：`Modbus TCP` / `Modbus RTU` / `MQTT` |
| `/api/v2/node/setting` | POST | `{"node":"n1","params":{…}}` | 驱动参数：`{"host","port","timeout","interval"}` |
| `/api/v2/node/ctl` | POST | `{"node":"n1","cmd":0}` | `0=START`、`1=STOP`（`msg.h` 的 `NEU_ADAPTER_CTL_START/STOP`） |
| `/api/v2/node` | GET | — | 列出节点：`{"nodes":[{"name":…}]}` |
| `/api/v2/node/state` | GET | — | `{"states":[{"node","running","link","rtt"}]}` |
| `/api/v2/group` | POST | `{"node","group","interval"}` | `interval` 是**整数毫秒** |
| `/api/v2/group?node=X` | GET | — | 读回验证用 |
| `/api/v2/tags` | POST | `{"node","group","tags":[{"type":3,"name","attribute":3,"address":"1!40001",…}]}` | `type=3`→INT16（`type.h`）；`attribute=1`READ / `2`WRITE / `3`两者 |
| `/api/v2/tags?node=X&group=Y` | GET | — | 读回验证用 |
| `/api/v2/read` | POST | `{"node","group"}` | 返回 `{"tags":[{"name","value"}]}` |
| `/api/v2/subscribe` | POST | `{"app":"mqtt-app","driver":"n1","group":"g1"}` | 把组订阅到北向应用 |

**关键陷阱**：MQTT 北向的 `driver-topic-prefix` 默认是 `neuron/${random_str}` ——
不显式设成固定值，话题名每次运行都不同，订阅端永远追不到。

**错误码**（`include/neuron/errcodes.h`，别猜语义）：
`1002 BODY_IS_WRONG`（请求体形状/字段名不对）、`1003 PARAM_IS_WRONG`、`1004 NEED_TOKEN`、
`1009 INVALID_USER_OR_PASSWORD`、`2002 NODE_EXIST`、`2003 NODE_NOT_EXIST`、`2006 NODE_NOT_READY`。

> 判据写成**容错式**：响应里没有 `error` 字段、或 `error==0`，都算成功 ——
> 因为 Neuron 某些成功响应根本不带 `error`。硬判 `== 0` 会把成功误判成失败。

## 可靠性内核（自研件③ 的核心，简历头条那条指标就来自这里）

`common/gw/reliability.h` + `apps/proxy_demo`。整条时序用**注入时钟**驱动，
所以「断网 10 分钟」是瞬间跑完的，可复现、可进 CI。

### 实测场景（默认参数）

```
=========== 边缘数据代理 · 断网续传场景 ===========
  采样周期      : 250 ms       总时长: 30 分钟
  断网          : 第 5 分钟起，持续 10 分钟
  队列容量 / TTL: 20000 条 / 60 分钟

时间轴:
  t=   0.0 min  Live        产出 0      积压 0
  t=   5.0 min  Backfill    产出 1201   积压 1        ← 链路断开，只落库
  t=  15.0 min  CatchingUp  产出 3606   积压 2149     ← 链路恢复，按 seq 排空
  t=  15.1 min  Live        产出 3615   积压 0

  产出 7200 条 / 接收去重 7200 条 / ★ 端到端丢失 0 / 重复 0
  积压峰值 2406 条（≈ 10min ÷ 250ms）/ 重连 1 次 / 退避上限 30000 ms
RESULT: 6 checks, 0 failed
```

### ★ 对账必须有两个口径，混用会得出假结论

这是我实现时**真实踩到并修掉**的问题：

| 口径 | 定义 | 局限 |
| -- | -- | -- |
| `missing_within_received()` | 以接收端见到的最大 seq 为界，衡量收到数据**内部**有无空洞 | **从没被发出去的数据它看不见** —— 发送端因容量/TTL 丢了尾部记录时，它照样报 0，制造「丢失 0」的假象 |
| `missing_against_produced(seq)` | ★ 生产端最高 seq − 接收端去重条数 | 「断网续传丢失 0」这句结论的**唯一合法依据** |

发现过程：容量打满的测试里 store 明明丢了 20 条，`missing_within_received()` 却报 0 ——
因为那 20 条从未发出，接收端的 max_seq 就只有 100。已把这条差别**写成专门的单测**钉住，
并在 `apps/README.md` 与头文件里写明。

### 三条反转断言（没有它们，「丢失 0」只是废话）

| 故障注入 | 实测结果 | 自洽性 |
| -- | -- | -- |
| `--drop-every 97`（静默丢包） | 端到端丢失 **74** | 7200÷97 ≈ 74.2 ✓ |
| `--capacity 100`（队列打满） | 端到端丢失 **2306** | 与 `store.dropped()=2306` 精确相等 ✓ |
| `--ttl-min 1`（TTL < 断网时长） | 端到端丢失 **2166** | 与 `store.expired()=2166` 精确相等 ✓ |

三条都证明：对账器在真丢数据时**会报错**，且报出的数量与丢弃原因逐一吻合。
第三行还固化了一条设计约束：**TTL 必须大于最长断网时间**。

## 真实 MQTT 传输（把假的传输换成 Paho + 真 broker）

`mqtt/` + `apps/mqtt_e2e`。`GW_WITH_MQTT=ON` 时构建。

### 实测：断网续传跑在真 MQTT 上

```
=========== MQTT 端到端自检（断网续传）===========
  最小 MQTT broker: 127.0.0.1:57955（真 TCP + 真 MQTT 帧）
  t=  3000 ms  —— 关掉 broker（模拟断网）——
  t=  3000 ms  Backfill    产出 151   积压 1
  t=  6000 ms  —— broker 恢复（同端口）——
  t=  6480 ms  Live        产出 325   积压 0

  生产 600 / broker 唯一接收 600 / ★ 端到端丢失 0 / 重复 0
  发布侧成功发送 600（重试 3）/ 积压峰值 175 / 重连 1 次 / broker 连接 2 次
RESULT: 8 checks, 0 failed
```

跨进程模式（A 窗口 broker 对账、B 窗口发布）同样两端一致：
发布侧 `200/200 未排空 0`、broker 侧 `唯一接收 200 / 丢失 0 / 重复 0`。

### 三种角色

```bash
mqtt_e2e --selftest                                  # 进程内自检（CI 用）
mqtt_e2e --broker --port 1884 --expect 2000          # 单独跑最小 broker，收满自动对账
mqtt_e2e --sub --host H --port P --expect 2000       # 订阅侧：接收端对账（对着真 broker 用）
mqtt_e2e --pub --host H --port P --count 2000 --outage-sec 5
```

对着**真 broker**（NanoMQ/docker）验证：`bash tools/mqtt_real_broker_wsl.sh`。

### 几个刻意的设计选择

| 点 | 选择 | 理由 |
| -- | -- | -- |
| Paho 绑定 | 用 **Paho MQTT C 的同步 API**（不是 C++ 包装） | `ITransport::send()` 是同步语义，C 的 `publishMessage + waitForCompletion` 与 QoS1 一一对应；C++ 的 Async 接口要额外事件循环 + 条件变量才能做成阻塞式，多一层可能出错的机器、多一个依赖 |
| 重连归属 | **状态机负责重连，不用 Paho 的自动重连** | 「断网续传」正是要验证的东西，重连节奏必须由我们的退避策略控制 |
| 持久化 | `MQTTCLIENT_PERSISTENCE_NONE` | 不让 Paho 的本地重发缓存掩盖我们自己队列的行为 |
| 载荷格式 | 固定 24 字节大端二进制（非 JSON） | 紧凑、无浮点、跨语言逐位一致，便于对账 |
| 断网模拟 | selftest 里**真的关掉 broker**（不是"告诉客户端断线"） | 更接近真实故障；`--pub` 对真 broker 时则用 `set_link_available(false)` 模拟网卡 down 事件 |

### ★ 实测约束：Paho 同步 API 单条发布 ~116 ms（已改变架构 → 已改造，见下文 R9 改造）

```
200 条、无断网、周期 1ms  →  墙上耗时 23322 ms  ⇒  平均 116 ms/条（≈8.6 条/秒）
```

这是 Paho 的 `waitForCompletion` 内部 **sleep(100ms) 轮询循环**造成的硬地板
（读 Paho C 1.3.13 源码实锤，`MQTTAsync.c` 与 `MQTTClient.c` 共用同一实现 ——
不是网络慢，broker 就在本机；也不是「同步客户端独有」，Async 的同名函数一样轮询）。
**它推翻了「一条消息发一个点位」的隐含假设**：若按方案 §7 的矩阵「500 点位 × 100ms 周期」，
一条一个点位就是 5000 条/秒 —— 在这个架构下**不可能**（上限 8.6 条/秒）。

正解有两条，**优先第一条**：

| 方案 | 做法 | 效果 |
| -- | -- | -- |
| ① **按采样周期批量打包**（推荐） | 一条 MQTT 消息携带该周期内 N 个点位的值 | 500 点位/100ms = **10 条消息/秒**，轻松满足。这也是真实网关的做法（Neuron 的 MQTT 北向就是按组发布，而非一点位一消息） |
| ② 换 Paho 的 **Async API** + 事件驱动等 PUBACK | 回调（Paho 线程）置状态，`send()` 条件变量等它 | ✅ **已落地**（R9 改造）：无需在途窗口即达标，语义零变化，见下文 |

⇒ 因此 **`RecordCodec` 需要扩展成批量格式**（一条消息 = 一个 seq + N 个点位），
并且压测指标口径要拆成两个数分开报：**点位/秒** 与 **消息/秒**。这是下一步的工作。

顺带一个代价说明（改造前）：`mqtt_e2e --selftest` 因此要跑 27 秒（150 条 × 116ms + 一次断网重连），
这是被测对象的真实成本，不是测试写得慢。**R9 改造后同样的自检只要 ~1.4 秒。**

### ★★ 实测：批量打包必要，但**不充分** —— 同步 API 的天花板 ~250 点位/秒

`mqtt_e2e --bench` 直接量（同一台机器、同一个 broker、相同总点位数）：

| 载荷 | 单条发布耗时 | 点位吞吐 | 消息吞吐 |
| -- | -- | -- | -- |
| 1 点位/消息 | 116 ms | **8.9 点位/秒** | 8.9 消息/秒 |
| 50 点位/消息 | ~200 ms | **250 点位/秒** | 5.0 消息/秒 |
| 100 点位/消息 | **471 ms** | 212 点位/秒 | 2.1 消息/秒 |

```
模式 A（1 点位/消息）  : 200 点位 / 200 条消息，22363 ms ⇒   8.9 点位/秒
模式 B（50 点位/消息）: 200 点位 /   4 条消息，  801 ms ⇒ 249.7 点位/秒
★ 点位吞吐提升 27.9×（消息数减少 50×）
RESULT: 5 checks, 0 failed
```

**三条结论**：

1. **打包是必需的**：一点位一消息只有 8.9 点位/秒，打包后提升 ~28×。
2. **但单条耗时随载荷增长**（116 → 200 → 471 ms），所以**最优批量在 50 点位附近**，
   再往上反而下降 —— 不能靠"无限加大批量"换吞吐。
3. ⇒ **同步 API 的天花板约 250 点位/秒**。方案 §7 矩阵里的
   「500 点位 @100ms」= **5000 点位/秒**，与此相差 **~20×**：
   **本实现达不到**，必须二选一 ——
   - 换 **Paho Async API** + 在途窗口（这是正解，符合真实网关做法）；
   - 或**据实下调指标**（例如把 100ms 周期只留给小规模场景，大规模用 1s 周期）。

> 这条数字必须诚实写进报告：它既证明了打包的价值，也划清了当前实现的边界。
> 假装能跑 5000 点位/秒比承认差 20 倍更危险 —— 面试官一压测就露。
> 【更新】这条边界在 2026-10-07 被 R9 改造拆除 —— 见下一节。

### ★★ R9 改造完成：Async API + 事件驱动等 PUBACK —— 天花板从 250 → 数万点位/秒

**根因修正**（比「换 API」更值钱的发现）：~116ms/条 的地板**不是**同步客户端独有，
而是 Paho C 1.3.13 的 `MQTTAsync_waitForCompletion` 源码里就是
`MQTTTime_sleep(100)` + 轮询 `isComplete` 的循环（`MQTTAsync.c:1418` 附近）——
同步/异步两个客户端共用同一个 100ms 轮询实现。先换 Async API 再逐条
`waitForCompletion`，实测地板纹丝不动（9.3 点位/秒，与同步版一致）。

**真正的修法**：不用 `waitForCompletion`，改注册 `onSuccess/onFailure` 回调
（PUBACK 到达即在 Paho 线程触发），`send()` 用 `std::condition_variable` 等它 ——
**事件驱动唤醒，零轮询**。`ITransport` 的对外语义原封不动：Ok 仍 = PUBACK 已确认。

改造后实测（同一台机器、同一 mini broker；Windows Debug 与 WSL Release 数字都在）：

| 口径 | 同步时代 | Async + 事件驱动 | 提升 |
| -- | -- | -- | -- |
| 1 点位/消息 | 8.9 点位/秒 | **1490 ~ 6126 点位/秒**（Win Debug / WSL Release） | **167 ~ 688×** |
| 30 点位/消息 | — | **59 826 点位/秒**（Win）/ **134 421 点位/秒**（WSL） | — |
| 100 点位/消息 | 212 点位/秒 | **357 633 点位/秒**（WSL Release） | **1687×** |

```
WSL Release（GCC 13.3 -O2）:
模式 A（1 点位/消息）  : 600 点位 / 600 条消息，耗时  98 ms ⇒   6125.7 点位/秒
模式 B（30 点位/消息）: 600 点位 /  20 条消息，耗时   4 ms ⇒ 134420.6 点位/秒
模式 B（100 点位/消息）: 600 点位 /   6 条消息，耗时   2 ms ⇒ 357632.5 点位/秒
RESULT: 5 checks, 0 failed
```

**三条结论（重写）**：

1. **100ms 地板消失**：单条发布从 ~116ms 降到 ~0.5ms（本地 broker），
   方案 §7 的「500 点位 @100ms = 5000 点位/秒」目标**被大幅超越**
   （30 点位/批 13.4 万点位/秒 ≈ 目标的 27×）——R9 的「据实下调指标」不再需要。
2. **打包依然有价值**：A/B 对比仍有 ~22-58× 的吞吐差（消息数少了，每条固定开销就少）；
   但它从「绕过 116ms 地板的唯一手段」降级为「常规优化」。
3. **断网续传语义零变化**：自检 18/18（丢失 0 / 重复 0 / 积压峰值 / 重连 1 次）
   在 Async 传输上原样通过，selftest 耗时从 27s 降到 ~1.4s。

过程坑（都修了）：e2e 订阅侧（--sub/--rawsub）同步移植到 Async 回调通道；
`MQTTAsync_disconnect` 吃 options 结构体而非裸超时；1.3.13 没有
`MQTTAsync_publish`（用 `MQTTAsync_send` + responseOptions 拿 token）；
bench 的 A/B 对比被偶发建连抖动翻转 ⇒ 计时前预热建连 + ctest 样本加大到 600 点位。

## W3-1 环形缓存：SQLite 持久化记录库（断电续传的落盘件）

内存版 `RecordStore` 的语义（容量满丢最新 / ack 回收 / TTL 过期）被抽成了
`IRecordStore` 接口，`store/` 给出 SQLite 落盘实现 —— 控制器与代理只认接口，
两个实现由同一组单测钉住同一份行为契约。

| 点 | 做法 | 为什么 |
| -- | -- | -- |
| 落库 | 每条记录一行（seq 主键 + ts_ms + points BLOB），WAL + synchronous=NORMAL | 进程被杀（SIGKILL）零丢失；「断电可能丢最后一笔」在环形缓存场景可接受（TTL 本来就丢），要更强改 FULL |
| seq 水位线 | 控制器每分配一个 seq 经 `note_seq()` 落 meta 表；重启后 `last_seq_hint()` 恢复 | **seq 绝不重用**是对账与去重的根基（`reliability.h` 里预留的注释由此兑现） |
| 接口隔离 | sqlite3.h 被 PIMPL 挡在 .cpp；直接用 C API 不引 SQLiteCpp | gw_common 保持零依赖；SQL 面积只有 5 条语句，多一个包装库不划算 |
| 断点续传 | `DataProxy`/`BackfillController` 构造新增 `seq_start` 参数，接线时传 `store.last_seq_hint()` | 重启后从断点之后继续分配，已落库未确认的记录原样排空 |

验收场景（`tests/test_sqlite_store.cpp::RestartResumeScenario`）：
断网产出 20 条 → 进程「崩溃」（不 flush，靠 SQLite 持久化）→ 重启后 20 条原样回来、
水位线 = 20 → 新数据从 seq 21 续采 → 积压排空 → **跨重启 Ledger 对账 30 条唯一、丢失 0、重复 0**。
构建：`GW_BUILD_SQLITE=ON`（WSL presets / CI 已开；Windows 本机默认关）。

## W3-2 多线程化：边缘代理服务壳（TSan 零竞态）

**架构选择：管道/actor 模型** —— 内核（DataProxy/Store/控制器/传输）只在管道线程上
被触碰，已验证的单线程语义**零改动、零锁**；多线程的全部同步都发生在边界：

```
采集线程（任意）                管道线程（唯一）                 Paho 线程（库内）
──────────────                ────────────────               ─────────────
produce(点位批) ──► BoundedQueue ──► DataProxy / Store / 控制器 ──► Async 发送
                     （互斥+cv）                                回调 ──► DeliverySlot(cv)
```

| 件 | 做法 | 为什么 |
| -- | -- | -- |
| `BoundedQueue`（header-only） | 互斥 + 双条件变量；push 不阻塞（满 ⇒ false + 计数），close 唤醒全部等待者 | 采集永不因网络/磁盘停摆；「丢弃计数」是采集侧丢失的独立口径 |
| `EdgeProxyService` | 采集 API `produce(点位批)`；管道线程排空队列 → `add_point/end_cycle`（一批一条，seq 保序）→ `tick()` 驱动退避/续传（真实时钟 `SteadyClock`） | seq 在管道线程分配 ⇒ 水位线/落库/发送天然串行 |
| 优雅停止 | `stop()` 关队列 ⇒ 管道排空剩余批次后退出（join） | 已收的批次不丢；store 里未确认记录原样保留（SQLite 断电续传语义） |
| 内核异常 | 管道 try/catch → `last_error()` 可见并停管道 | SQLite 损坏这类故障不许静默吞 |

### TSan 实测（wsl-tsan preset，101/101 零警告）

1. **TSan 首战抓到的是测试自己的竞态**：对账器 Ledger 在真实系统里住「云端」（另一进程），
   测试里共享内存化后，主线程轮询 `unique_count()` 与管道线程的 `accept()` 裸竞争
   （`_Rb_tree::size()` 数据竞争，TSan 报告实锤）—— 修法：测试侧一律经互斥快照读。
   这条教训值钱：**测试装置的并发正确性也是并发正确性**。
2. **TSan × 新内核 ASLR 启动崩溃**：`unexpected memory mapping` 随机出现 ——
   `setarch -R` 禁 ASLR 规避（CI 已加）。
3. **GCC 13 的 `-Wtsan` 告警**：libstdc++ 的 `atomic_thread_fence` 不被 TSan 建模，
   asio 头触发 ⇒ wsl-tsan preset 瘦身（关掉 sim/mqtt/driver，TSan 只跑纯逻辑测试，
   与 asan job 同一哲学）+ `-Wno-tsan`。

### 三个实测撞出来的坑（都是真 bug，不是配置问题）

1. **PUBACK 超时后必须主动拆连接**：否则 paho 的 `isConnected()` 可能长期返回真（要等下一次 IO 才发现 socket 死了），我们就永远不重连，状态机卡在 Backfill 空转。
2. **asio 的 `io.run()` 会在没有未完成工作时立刻返回**：broker 一停，io 线程直接退出，之后 `post` 进去的东西永远不执行 → 主线程死等。必须用 `make_work_guard` 保住。
3. **asio 对象不是线程安全的**：从主线程直接调 broker 的 `stop()/start()`，会在 `async_accept` 在途时跨线程改状态（实测表现为"重启后 broker 其实没监听上"）。必须 `post` 进 io 线程并等其执行完。
4. **~116ms 地板的真凶是 `waitForCompletion` 自己**：Paho C 1.3.13 里同步/异步两个客户端的该函数都是 `sleep(100ms)` 轮询循环（源码 `MQTTAsync.c` 实锤）。先换 Async API 逐条 `waitForCompletion`，地板纹丝不动（9.3/s）—— 事件驱动等 PUBACK 后才消失。教训：性能归因要读到库源码那一层，"换了 API 就该快"是猜测不是证据。

### CMake 侧的坑

- **Paho 的头必须走 SYSTEM include**（同 asio）。
- **链接第三方静态库时，库路径不能含非 ASCII 字符**：ninja 的链接步骤要经 `cmd.exe`（中文 Windows 用 GBK 代码页），UTF-8 路径会被打乱，ld 报 `cannot find .../妗岄潰椤圭洰/...`。编译步骤不经 cmd.exe，所以 `-isystem` 的中文路径一直正常 —— 只有链接会中招。⇒ 把本地依赖放到纯 ASCII 路径（如 `~/` 下）。
- **从源码编 Paho（v1.3.13）的两个前提**：`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`（它的 `cmake_minimum_required` 太老，CMake 4 拒绝），以及 `-DCMAKE_C_STANDARD=99 -DCMAKE_C_FLAGS=-std=gnu99`（它的 `MQTTPacket.h` 里 `typedef bool` 与 C23 关键字冲突）。

## 自研件② Neuron 驱动插件（C/C++ ABI 边界）

`plugins/driver/`。`GW_BUILD_DRIVER=ON` 时构建；不传 `GW_NEURON_SDK_DIR` 就走内置 SDK 垫片。

**结构（这不是风格选择，是硬约束）**：`plugin_module.c`（薄 C 层，只定义模块描述符）
+ `driver_impl.cpp`（全部业务逻辑）。原因见 `plugins/README.md`：
`neu_plugin_intf_funs_t` 的 `.driver = { … }` 是**嵌套指定初始化器**，
C99 合法但 **C++20 明确禁止**，且 `neu_plugin_module_t` 成员带 `const` 无法后赋值
⇒ 纯 C++ 会在填描述符这步直接编译失败。

### 实测：扮演一个最小 Neuron 来驱动它

`plugins/driver/tests/abi_check.cpp` 就是「假 Neuron」：`LoadLibrary` →
`GetProcAddress("neu_plugin_module")` → 读描述符 → `open/init/setting` → `validate_tag`
→ `start` → `driver.group_timer` → `write_tag` → `stop/uninit/close`。

```
driver_check --lib <libgw_driver.dll>                        #  22 checks, 0 failed（无需设备，进 CI）
gw_sim --port 15020 & driver_check --lib <…> --device-port 15020   #  30 checks, 0 failed（完整链路）
```

后者是**完整链路**：真实设备 → `NgwpClient` → 插件 → `adapter_callbacks->update` 回调。
关键断言：嵌套 struct 里的 `driver.group_timer` 指针非空（C 结构体布局正确）、
`group_timer` 上报 4 条、写 0x1234 后回执 `error==0` 且写后读一致、
`group_timer(NULL)/setting(NULL)/validate_tag(NULL)` 返回错误码**而不崩溃**（异常没穿越边界）。

> 为什么用垫片而不是等真 Neuron：把「代码问题」与「SDK 环境问题」分开。
> 插件出问题时只剩 SDK/ABI 一类原因，不会再混进协议问题。
> **但垫片保真的只是字段名与签名** —— 别把「垫片过了」当成「真 SDK 一定编得过」，
> 差异清单写在 `plugins/README.md`。

## 设计要点（面试可讲）

| 点 | 做法 | 为什么这样做 |
| -- | -- | -- |
| TCP 粘包/拆包 | 显式状态机；阶段用 `std::variant<WaitMagic, WaitLen, WaitBody>`，事件用 `std::variant<DecodedFrame, DecodeError>`，`std::visit` 分派 | 状态与事件都是类型，漏处理某个分支编译器直接报错 |
| 重同步 | 做成 `ResyncPolicy` 两档（CRC 失败时整帧跳过 / 逐字节扫描），各有取舍，实测噪声 1 vs 13 | 「没有免费答案」的取舍显式化，而不是藏一个魔法常量 |
| CRC | 位运算参考实现 + 256 项查表实现**互相交叉验证**，并与 CRC-16/MODBUS 标准校验值 `0x4B37` 对齐 | 两份独立实现互证，不是自我验证 |
| 抗内存放大 | 先校验 `LEN <= 261` 再分配缓冲 | 不校验就等于让对端用一个 4 字节字段指定任意大的分配量 |
| 确定性 | 自定义 splitmix64，不用 `<random>`；同 seed 的整数序列跨平台逐位一致 | `uniform_int_distribution` 的实现不保证跨标准库一致，会让重放失效 |
| 文档与代码对齐 | 标准向量由程序导出 + 单测逐字节钉住 | 手写的测试向量迟早会和实现对不上，而这件事本身没人会发现 |
| 环境与代码解耦 | 内置 GoogleTest 兼容垫片 | 「代码对不对」与「环境装没装」必须能分开验证 |
| 长期不变的检查器 | 对账必须同时用「端到端」与「接收侧自洽」两个口径 | 只用后者会把「从未发出的数据」当成没丢（实测踩过） |
| 测试登记 | `ctest -N` 核对条数 | `enable_testing()` 若晚于 `add_test`，测试会被**静默丢弃**；「ctest 通过」不等于「测试都登记了」 |
| 跨平台互检（第二例） | 静态库不带 `-fPIC` 在 MinGW 上永远编不出错，Linux 链接 `.so` 才炸 ⇒ 全库 `POSITION_INDEPENDENT_CODE ON`（`df21fd4`） | WSL 复验抓到：互检矩阵每多一维（编译器/平台/优化级别/sanitizer）就能多抓一类问题；CI 的 wsl preset 修复前一旦推送必挂 |

## 已知边界（诚实声明）

- **Sine 策略理论上不保证跨平台逐位一致**（走 libm 的 `std::sin`，理论上不同实现可有末位差异）——
  但**实测在 g++ / clang++、MinGW / Linux、`-O0` / `-O2` 五种组合下哈希完全相同**（见上表）。
  所以这里保留的是**理论风险声明**，不是已知失败。整数路径（`Constant` / `RandomWalk`）
  从算法上就与 libm 无关。
- 演示里的重放哈希用的是「正弦 + 随机游走」混合流。早期版本只挂正弦 —— 而正弦无状态、
  不消耗 RNG，导致「换 seed 字节流不变」却在最初被判为通过；已修（混入一个随机游走点位）。
- 插件已完成全链路实测：本机垫片路线（22/22 + 30/30）⇒ WSL 真 SDK 头构建（漂移复核 528 常量一致）⇒ **真 Neuron 加载建节点跑通**（REST 建节点/写参数/建组/建点位 error 0，连续读回正弦活数据；`tools/ngwp_node_rest.sh` 可复现）。踩坑实录见 `plugins/README.md`。
- Windows 本机：llvm-mingw 的 libc++ 与本机 asio 组合，运行期 `std::thread` 会抛 `system_error`
  （已用最小程序隔离确认是工具链交互问题，非本项目代码）。故本机 `gw_sim --selftest`
  只由 g++ 运行；WSL/Linux 不受此限。详见 `apps/README.md`。
- 协议层的异常（`EncodeError`）刻意不跨越未来插件的 `extern "C"` 边界 —— 见方案 §5②。
