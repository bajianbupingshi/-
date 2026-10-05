# 工业网关自定义协议 NGWP v1（一页定稿）

> **NGWP** = Neuron Gateway Wire Protocol。类 HSMS 风格：定长头 + 长度前缀 + 功能码 + CRC。
> 设计目标：一条 TCP 连接上承载寄存器读写；**粘包/拆包必须可恢复**；畸形报文不得撑爆内存。
> 本文是实现的**唯一契约**，代码与本文件不一致时以本文件为准（并立即改代码）。

## 1. 字节序与基本假设

- **全部多字节字段为大端（网络字节序）**。寄存器值为 16 位无符号，大端。
- 传输层为 TCP，**流式、无消息边界**：一次 `recv` 可能拿到 0.5 帧、1 帧或 3 帧。
- 单帧最大 **269 字节**（见 §4 约束）。任何超出即判为畸形。

## 2. 帧格式

| 偏移 | 长度 | 字段 | 说明 |
| -- | -- | -- | -- |
| 0 | 2 | `MAGIC` | 固定 `0x47 0x57`（ASCII `"GW"`） |
| 2 | 4 | `LEN` | 后续字节数 = `5 + payload_len`，**不含** MAGIC/LEN/CRC 自身 |
| 6 | 1 | `FUNC` | 功能码，见 §3 |
| 7 | 2 | `ADDR` | 寄存器起始地址（0 基） |
| 9 | 2 | `QTY` | 寄存器数量（`READ_IDENTITY` 固定为 0） |
| 11 | `payload_len` | `PAYLOAD` | 寄存器值数组或异常码，见 §3 |
| 11+`payload_len` | 2 | `CRC16` | CRC-16/MODBUS，覆盖 **偏移 0 … PAYLOAD 末字节** |

**帧总长 = 8 + LEN**；`payload_len = LEN - 5`。

## 3. 功能码与载荷语义

`FUNC` 最高位为**响应/异常标志位**：`FUNC & 0x80` 表示这是异常响应。

| 码 | 名称 | 请求 PAYLOAD | 响应 PAYLOAD | 备注 |
| -- | -- | -- | -- | -- |
| `0x01` | `READ_HOLDING` | 空 | `2*QTY` 字节 | 读保持寄存器 |
| `0x02` | `READ_INPUT` | 空 | `2*QTY` 字节 | 读输入寄存器 |
| `0x03` | `WRITE_SINGLE` | 2 字节 | 回显 2 字节 | 要求 `QTY == 1` |
| `0x04` | `WRITE_MULTI` | `2*QTY` 字节 | 回显 `2*QTY` 字节 | — |
| `0x05` | `READ_IDENTITY` | 空 | ≤256 字节 ASCII | 要求 `QTY == 0`；载荷长度不受 QTY 约束 |

异常响应（`FUNC \| 0x80`）的 `PAYLOAD` 恰好 1 字节：

| 码 | 名称 | 含义 |
| -- | -- | -- |
| `0x01` | `ILLEGAL_FUNCTION` | 不支持的功能码 |
| `0x02` | `ILLEGAL_ADDRESS` | 地址越界 |
| `0x03` | `ILLEGAL_QUANTITY` | 数量越界（`QTY` 为 0 或 > 64） |
| `0x04` | `DEVICE_FAILURE` | 设备内部错误 |

## 4. 结构与长度约束

| 常量 | 值 | 理由 |
| -- | -- | -- |
| `MAX_QTY` | 64 | 单帧最多 64 个寄存器（128 字节载荷），避免单帧过大抢占链路 |
| `MAX_PAYLOAD` | 256 | `READ_IDENTITY` 上限 |
| `MAX_LEN` | 261 | `5 + MAX_PAYLOAD` |
| `MAX_FRAME` | 269 | `8 + MAX_LEN` |

**解析器的结构判据**（不满足即 `BAD_STRUCTURE`，不进入业务层）：

1. `LEN >= 5` 且 `LEN <= MAX_LEN`（否则 `LENGTH_TOO_LARGE`）；
2. 非异常帧：`payload_len == 0 || payload_len == 2*QTY`；
3. `FUNC` 为 `READ_IDENTITY` 时豁免第 2 条，改为 `QTY == 0 && payload_len <= MAX_PAYLOAD`；
4. 异常帧：`payload_len == 1`；
5. 寄存器类功能码要求 `1 <= QTY <= MAX_QTY`（`READ_IDENTITY` 除外，见 3）。

**边界划分（重要）**：解析器只判**形状**，不判**语义**。
`FUNC` 是未知值时（例如 `0x7E`），只要形状合法（`payload_len <= MAX_PAYLOAD`）就照常解析出来，
由业务层回 `ILLEGAL_FUNCTION` 异常帧 —— 这样对端能拿到明确的错误码，而不是被静默丢包。

> 先校验 `LEN` 再分配缓冲，是本协议**抗内存放大攻击**的关键：不校验就等于让对端用一个 4 字节字段指定任意大的分配量。

## 5. CRC-16/MODBUS

| 参数 | 值 |
| -- | -- |
| 多项式 | `0x8005`（反射后 `0xA001`） |
| 初值 | `0xFFFF` |
| 输入/输出反射 | 是 / 是 |
| 结果异或 | `0x0000` |
| **标准校验值** | 对 ASCII `"123456789"` 应得 **`0x4B37`** |

CRC 在线路上**低字节在前**（与 Modbus RTU 一致）。实现提供两份代码：位运算参考实现与 256 项查表实现，
单测强制二者在所有用例上逐位一致 —— 这是「两份独立实现互证」，不是自我验证。

## 6. 解析器状态机与重同步

解析器是一个**显式状态机**：阶段用 `std::variant<WaitMagic, WaitLen, WaitBody>` 表达，
产出事件用 `std::variant<DecodedFrame, DecodeError>` 表达，两者都经 `std::visit` 分派。

```
WaitMagic ──魔数命中──▶ WaitLen ──LEN 到齐──▶ WaitBody ──帧到齐──▶ CRC + 结构校验
     ▲                      │                    │
     └──────── 出错：按 ResyncPolicy 处理 ────────┘
```

**重同步策略是显式可配的**（`ResyncPolicy`），因为这里没有免费答案：

| 策略 | 适用 | 行为 | 代价 |
| -- | -- | -- | -- |
| `kSkipFrameOnCrcError`（默认） | CRC 失败 | 跳过整帧（此时帧头与帧长都已可信） | 若帧内的 `0x47 0x57` 只是载荷巧合，可能连带跳过紧随其后的真帧；损害上界为 `MAX_FRAME` 字节 |
| `kScanByByte` | 任意错误 | 只丢弃 1 个字节，重新扫描魔数 | 一个坏帧会沿路吐出一串 `BAD_MAGIC` 噪声事件 |

魔数不符 / 长度非法时，两种策略都只丢弃 1 个字节（此时帧长不可信，不能按帧跳）。

- 合法帧一帧一帧产出，**一次 `feed()` 可吐出多帧**（粘包）。
- **逐字节喂入与一次性喂入必须产出完全相同的事件序列**（拆包等价性，由单测强制）。

实测对比（`proto_demo` 第 4 节：2 tick × 3 次读 × 2 帧 = 12 帧的流，破坏其中第 1 帧的 1 个 bit）：

| 策略 | 解出帧数 | 错误事件数 |
| -- | -- | -- |
| `kSkipFrameOnCrcError` | 11 | 1 |
| `kScanByByte` | 11 | 13 |

两者都**没有漏掉后续帧**，差别只在噪声量级 —— 这正是「策略可配」的价值。

## 7. 标准测试向量

**下列字节由 `proto_demo --vectors-only` 从实现直接导出，并有单测 `Frame.标准向量逐字节钉住_与协议文档保持一致` 逐字节钉住，禁止手写。**
（手写的向量迟早会和实现对不上，而「对不上」这件事本身没人会发现。）

| 帧 | 长度 | 线路字节 |
| -- | -- | -- |
| `READ_HOLDING` 请求 addr=0x0000 qty=10 | 13 | `47 57 00 00 00 05 01 00 00 00 0A 46 58` |
| `WRITE_SINGLE` 请求 addr=0x0004 val=0x1234 | 15 | `47 57 00 00 00 07 03 00 04 00 01 12 34 1C F6` |
| `READ_HOLDING` 响应 addr=0x0000 [1,2,3] | 19 | `47 57 00 00 00 0B 01 00 00 00 03 00 01 00 02 00 03 83 BD` |
| 异常响应 addr=0x0000 qty=10 code=ILLEGAL_ADDRESS | 14 | `47 57 00 00 00 06 81 00 00 00 0A 02 74 F3` |
| `READ_IDENTITY` 响应 `"NGWP-SIM/1.0"` | 25 | `47 57 00 00 00 11 05 00 00 00 00 4E 47 57 50 2D 53 49 4D 2F 31 2E 30 12 D0` |

以第一行为例逐字段拆开：

```
47 57              MAGIC
00 00 00 05        LEN = 5  ( = 5 + payload_len(0) )
01                 FUNC = READ_HOLDING
00 00              ADDR = 0x0000
00 0A              QTY  = 10
46 58              CRC  = 0x5846，低字节 0x46 在前
```

复现命令：

```bash
cmake --build build-host-gcc -j
./build-host-gcc/apps/proto_demo/proto_demo --vectors-only
```

