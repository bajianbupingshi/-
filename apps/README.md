# apps/（可执行程序）

- `proto_demo/` —— **已完成**：协议层演示（编码 → 解析 → 注入 → 确定性重放）。
- `sim/` —— **已完成（W2 自研件①）**：基于 asio 的 NGWP 设备模拟器。

## sim —— 设备模拟器

一台用自定义类 HSMS 协议对外提供寄存器读写的设备。它只负责「收字节 → 喂解析器 →
向 DeviceModel 要响应 → 把响应当字节发回去」，协议与语义逻辑全部复用 `gw_common`。

```
gw_sim --port 1502 --points 12 --interval 100        # 正常服务
gw_sim --selftest                                    # 进程内端到端自检（已登记进 CTest）
gw_sim --help
```

**线程模型**：单线程 `io_context` ⇒ `DeviceModel` 与寄存器表无需加锁。
要上多线程，必须先给每个会话加 strand，或给模型加锁 —— 不要直接 `io.run()` 开多线程。

**写队列是必需的**：若复用同一块发送缓冲，第二次读触发的写会覆盖第一次尚未发完的缓冲
（典型 use-after-free）。实现里用 `std::deque<std::shared_ptr<std::vector<uint8_t>>>` 串行化。

### 自检覆盖（不需要任何外部客户端）

自己起服务器（内核分配空闲端口）+ 自己连，跑 6 组端到端用例：

| # | 用例 | 断言要点 |
| -- | -- | -- |
| ① | 单帧往返 | 非异常、qty=4、载荷 8 字节、逐值与设备寄存器表一致 |
| ② | 拆包 | 逐字节发送仍拼出完整帧并正确响应 |
| ③ | 粘包 | 一次发两帧 → 收到两个响应，功能码顺序正确 |
| ④ | 写入生效 | 写单个回显一致；读回一致；服务端寄存器表确实被更新 |
| ⑤ | 语义错误 | 越界读回 `ILLEGAL_ADDRESS` 异常帧（而不是静默丢弃） |
| ⑥ | 坏帧重同步 | 坏 CRC 帧被静默丢弃，紧随其后的合法帧仍被应答 |

实测：**23 checks, 0 failed**（g++ 16.2.0 / MinGW-W64，Windows）。

### 已知工具链限制（Windows 本机，与真目标无关）

llvm-mingw 的 libc++ 与这份 asio 组合，**运行期 `std::thread` 构造会抛 `system_error`**。
已用最小程序隔离确认：纯 `std::thread` 程序 clang++ 正常；最小 asio+thread 程序
clang++ 崩、g++ 正常 ⇒ 是工具链交互问题，不是本项目代码问题。
所以本机 `build_host.sh` 只让 g++ 运行该自检；clang++ 仍参与**编译检查**
（它抓到过 g++ 漏报的 `-Wunused-private-field`）。真目标 WSL/Linux 不受此限。

**另一个坑**：asio 自己的头文件在 `-Wshadow` 下有告警
（如 `detail/impl/win_static_mutex.ipp` 里 `HANDLE mutex` 遮蔽全局 `mutex`），
而本项目全局开 `-Werror` ⇒ asio 的 include 目录**必须**用
`target_include_directories(... SYSTEM ...)`，否则会被第三方代码卡死编译。
