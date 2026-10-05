# plugins/driver —— Neuron 驱动插件（自研件②）

**状态：已实现并实测**（本机走 SDK 垫片验证；对真 Neuron 的构建见文末 WSL 说明）。

## 文件与分工

| 文件 | 语言 | 职责 |
| -- | -- | -- |
| `plugin_module.c` | **C** | 只做一件事：定义 `neu_plugin_intf_funs_t` 与 `neu_plugin_module`（模块描述符） |
| `driver_impl.cpp` | C++ | 全部业务逻辑：连设备、周期读点位、上报、写点位 |
| `driver_impl.h` | C 头 | C/C++ 边界契约（全部 `extern "C"`） |
| `sdk_shim/neuron/plugin.h` | C 头 | 仅本机验证用的 Neuron SDK 垫片（**逐字抄**真实关键结构体） |
| `tests/abi_check.cpp` | C++ | 「假 Neuron」：动态加载插件并驱动完整生命周期 |

## ★ 为什么描述符必须是 C 编译单元（实测结论）

`neu_plugin_intf_funs_t` 内部是 `union { struct { … } driver; }`，官方示例写法是
`.driver = { .validate_tag = …, .group_timer = … }` —— 这是**嵌套指定初始化器**：

- C99：合法；
- **C++20：明确禁止嵌套**（`dcl.init.aggr`：designated-initializer-list shall not be nested），
  且要求 designator 顺序与声明顺序一致、不能跳成员；
- 再加上 `neu_plugin_module_t` 的成员带 `const`，**无法「先默认构造再赋值」**。

⇒ 纯 C++ 写插件会在「填描述符」这步直接编译失败。所以插件的结构是
**一层薄 C（描述符）+ 全部 C++（逻辑）**，两边各取所长：C 侧拿到编译期常量描述符，
C++ 侧能用 RAII / `std::string` / 异常（在边界内消化）。

**这正好是简历里那个 ABI 面试点的实物**：C++ 对象怎么跨动态库边界、dlopen 后工厂函数
怎么返回对象、为什么异常不能穿越 `extern "C"`。

## ★ C/C++ 边界的硬规则

1. 全部导出函数用 `extern "C"`（不被 mangle，C 侧才引用得到）。
2. **任何 C++ 异常都不许穿越边界**：每个函数的实现都是 `try { … } catch (...) { return -1; }`。
   跨语言边界的异常行为未定义，而且上层 Neuron 内核是 C，没有 `catch` 的概念。
   自检里有专门一条：`group_timer(NULL)` / `setting(NULL)` / `validate_tag(NULL)`
   都必须返回错误码而**不是崩溃**。
3. 插件实例布局：`struct neu_plugin` 的第一个成员必须是 `neu_plugin_common_t`
   （真实 SDK 里 `neu_plugin` 也由各插件自己定义，垫片同样只做前置声明）。

## 本机实测（`driver_check`）

**扮演最小 Neuron**：`LoadLibrary` → `GetProcAddress("neu_plugin_module")` → 读描述符 →
`open/init/setting` → `validate_tag` → `start` → `driver.group_timer` → `write_tag`
→ `stop/uninit/close`。

```bash
# 只验 ABI + 生命周期（无需设备，可进 CI）
driver_check --lib <插件库路径>                        # 22 checks, 0 failed

# 完整链路：真设备 → 客户端 → 插件 → update 回调
gw_sim --port 15020 --interval 600000 &               # 起真实设备
driver_check --lib <插件库路径> --device-port 15020     # 30 checks, 0 failed
```

实测关键断言：

| 断言 | 意义 |
| -- | -- |
| `GetProcAddress("neu_plugin_module") != NULL` | 符号导出正确（Neuron 就是 dlsym 它） |
| 描述符 `name=NGWP Sim / type=DRIVER / kind=SYSTEM` | 跨编译单元读到的内存一致 |
| **`funs->driver.group_timer != NULL`（嵌套 struct 里的函数指针）** | ★ C 结构体布局正确 |
| `group_timer` 返回 0 且上报 4 条（`t0..t3`） | ★ 回调链真的通了 |
| `write_tag` 后 `write_response` 被回调且 `error==0` | 写回执路径正确 |
| 写 0x1234 后再 `group_timer`，上报值 = 0x1234 | 写后读一致 |
| `group_timer(NULL)` / `setting(NULL)` / `validate_tag(NULL)` 返回错误 | ★ 异常没穿越边界 |

## 已知简化（诚实边界）

- **settings 解析**：真实 Neuron 给一段 JSON，生产实现应当用 SDK 的 JSON 接口或 schema 驱动；
  这里只手工抽了 `host/port/timeout` 三个字段，够跑通。
- **单点位逐个读**：`group_timer` 目前每个点位一次请求，没有合并连续地址。
  （协议支持一次读多个寄存器；合并是明确的下一步优化。）
- **垫片的保真范围**：核心结构体逐字抄；`neu_plugin_common_t` / `adapter_callbacks_t` /
  `neu_dvalue_t` / `neu_value_u` 只保留用到的字段 ——
  保真的是**字段名与签名**（插件一律按名字访问，所以垫片与真 SDK 都能编过）。
  **不要把「垫片过了」当成「真 SDK 一定编得过」**：真 SDK 上仍需处理的差异见下。

## 对真 Neuron 构建（WSL，待执行）

```bash
# 1) 取 SDK（开源 releases 只发 SDK，不发预编译二进制）
cd ~/neuron && wget https://github.com/emqx/neuron/releases/download/v2.16-daily/neuron-sdk-2.16.0-amd64.tar.gz
tar xzf neuron-sdk-2.16.0-amd64.tar.gz && sudo ./neuron-sdk-2.16.0/sdk-install.sh

# 2) 用真 SDK 头 + 链接 libneuron-base 构建插件
cmake -B build-plugin -G Ninja -DGW_BUILD_DRIVER=ON \
      -DGW_NEURON_SDK_DIR=/usr/local/include/neuron \
      -DGW_ASIO_INCLUDE_DIR=<asio>/include
cmake --build build-plugin -j"$(nproc)"

# 3) 把 .so 与 schema 拷到 Neuron 的 plugins 目录并登记
cp build-plugin/plugins/driver/libgw_driver.so ~/neuron/build/plugins/
cp ngwp-sim.json ~/neuron/build/plugins/schema/     # schema 文件待补
```

**尚未做**：`ngwp-sim.json`（settings schema）、`plugins.json` 登记、在真 Neuron 里建节点跑通。
这需要在 WSL 里执行（我这边 `wsl.exe` 被安全策略拦）。
