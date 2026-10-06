// NGWP 驱动插件 —— C++ 实现层。
//
// 分工：plugin_module.c（C 编译单元）负责填 neu_plugin_module_t / neu_plugin_intf_funs_t；
//       本文件负责全部业务逻辑：连设备、周期读点位、上报、写点位。
//
// ★ 为什么业务逻辑不在 C 层：neu_plugin_intf_funs_t 的初始化必须用 C99 的**嵌套指定初始化器**
//   （`.driver = { .validate_tag = ... }`），而 C++20 明确禁止嵌套指定初始化器、且要求
//   声明顺序。所以模块描述符只能由 C 编译单元定义 —— 这是硬约束，不是风格选择。
//   反过来说，逻辑放在 C++ 里能用 RAII / std::string / 异常（在边界内消化），
//   两边各取所长。这条边界本身就是简历里那个 ABI 面试点的实物。

#include "driver_impl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gw/client.h"
#include "gw/frame.h"

// ── 每个插件实例的状态（C++ 侧）─────────────────────────────────────────────
namespace {

struct DriverState {
    std::string host = "127.0.0.1";
    std::uint16_t port = 1502;
    std::uint32_t timeout_ms = 1000;
    bool started = false;

    gw::NgwpClient client{gw::ClientConfig{"127.0.0.1", 1502, 1000, 2000}};

    std::uint64_t reads = 0;
    std::uint64_t updates = 0;
    std::uint64_t errors = 0;
    std::string last_error;
};

// 全局统计（供测试观察；真实插件不需要）
std::uint64_t g_reads = 0;
std::uint64_t g_updates = 0;
std::uint64_t g_errors = 0;
std::string g_last_error;

void note_error(DriverState *st, const std::string& what) {
    if (st != nullptr) {
        ++st->errors;
        st->last_error = what;
    }
    ++g_errors;
    g_last_error = what;
}

// ── 极简 settings 解析 ──────────────────────────────────────────────────────
// 真实 Neuron 会给一段 JSON；生产实现应当用 SDK 的 JSON 接口（或 schema 驱动）。
// 这里只抽 host/port/timeout 三个字段，够跑通；属于**已知简化**，已在 README 声明。
bool json_get_string(const std::string& json, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    std::size_t p = json.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) {
        return false;
    }
    std::size_t q = json.find('"', p);
    if (q == std::string::npos) {
        return false;
    }
    const std::size_t e = json.find('"', q + 1);
    if (e == std::string::npos) {
        return false;
    }
    out = json.substr(q + 1, e - q - 1);
    return true;
}

bool json_get_uint(const std::string& json, const char* key, std::uint64_t& out) {
    const std::string pat = std::string("\"") + key + "\"";
    std::size_t p = json.find(pat);
    if (p == std::string::npos) {
        return false;
    }
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(json.c_str() + p + 1, &end, 10);
    if (end == nullptr || end == json.c_str() + p + 1) {
        return false;
    }
    out = v;
    return true;
}

// 点位地址格式：`<slave>!<register>`（与 Neuron Modbus 驱动一致），或直接给寄存器号。
// 例：`1!40001` → 寄存器 40001（4xxxx 保持寄存器，base 40001 → 偏移 0）。
struct ParsedAddress {
    bool ok = false;
    std::uint16_t reg = 0;   // 归一化后的寄存器偏移
};

ParsedAddress parse_address(const char* addr) {
    ParsedAddress out;
    if (addr == nullptr) {
        return out;
    }
    const char* bang = std::strchr(addr, '!');
    const char* num = (bang != nullptr) ? bang + 1 : addr;
    const unsigned long raw = std::strtoul(num, nullptr, 10);
    if (raw == 0) {
        return out;
    }
    // 4xxxx 保持寄存器 → 偏移 = raw - 40001（与官方 Quick Start 的 1!40001 对应）
    if (raw >= 40001UL) {
        out.reg = static_cast<std::uint16_t>(raw - 40001UL);
    } else {
        out.reg = static_cast<std::uint16_t>(raw);
    }
    out.ok = true;
    return out;
}

// C++ 实例伪装成 neu_plugin_t：**第一个成员必须是 neu_plugin_common_t**，
// 这样 (neu_plugin_t*)p 与 (GwPlugin*)p 指向同一地址（标准布局首成员规则）。
struct GwPlugin {
    neu_plugin_common_t common;
    DriverState*        st;
};

GwPlugin* as_plugin(neu_plugin_t* p) {
    return reinterpret_cast<GwPlugin*>(p);
}

}  // namespace

// 与 Neuron src/base/neu_plugin_common.c 一致（真 SDK 头文件不导出该宏，
// 官方的 neu_plugin_common_init() 是宿主符号 —— .so 里引它会要求宿主导出；
// 直接赋值让插件零外部符号依赖，driver_check 假 Neuron 也能独立加载）
#ifndef NEU_PLUGIN_MAGIC_NUMBER
#define NEU_PLUGIN_MAGIC_NUMBER 0x43474D50u  // "PMGC"
#endif

// ── 生命周期 ────────────────────────────────────────────────────────────────
extern "C" neu_plugin_t* gw_driver_open(void) {
    try {
        auto* p = new (std::nothrow) GwPlugin();
        if (p == nullptr) {
            return nullptr;
        }
        std::memset(&p->common, 0, sizeof(p->common));
        // ★ common.magic 必须是 NEU_PLUGIN_MAGIC_NUMBER —— Neuron 建节点时
        //   neu_plugin_common_check() 只认它（P0-3 真机联调实测：magic 为 0
        //   会让 neu_adapter_create 的 assert 把整个 Neuron 进程打崩）。
        p->common.magic = NEU_PLUGIN_MAGIC_NUMBER;
        p->st = new (std::nothrow) DriverState();
        if (p->st == nullptr) {
            delete p;
            return nullptr;
        }
        std::snprintf(p->common.name, sizeof(p->common.name), "%s", "NGWP Sim");
        return reinterpret_cast<neu_plugin_t*>(p);
    } catch (...) {
        // 边界内消化异常：不向 C 调用方抛出
        return nullptr;
    }
}

extern "C" int gw_driver_close(neu_plugin_t* plugin) {
    try {
        if (plugin == nullptr) {
            return -1;
        }
        GwPlugin* p = as_plugin(plugin);
        delete p->st;
        p->st = nullptr;
        delete p;
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int gw_driver_init(neu_plugin_t* plugin, bool /*load*/) {
    try {
        return plugin == nullptr ? -1 : 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int gw_driver_uninit(neu_plugin_t* plugin) {
    try {
        if (plugin == nullptr) {
            return -1;
        }
        as_plugin(plugin)->st->client.close();
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int gw_driver_start(neu_plugin_t* plugin) {
    try {
        if (plugin == nullptr) {
            return -1;
        }
        DriverState* st = as_plugin(plugin)->st;
        st->client.reconfigure(gw::ClientConfig{st->host, st->port, st->timeout_ms, 2000});
        if (!st->client.connect()) {
            note_error(st, "start: 设备连接失败 " + st->client.last_error());
            return -1;   // 真实 Neuron 允许 start 失败；组定时器不会触发
        }
        st->started = true;
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int gw_driver_stop(neu_plugin_t* plugin) {
    try {
        if (plugin == nullptr) {
            return -1;
        }
        DriverState* st = as_plugin(plugin)->st;
        st->client.close();
        st->started = false;
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int gw_driver_setting(neu_plugin_t* plugin, const char* setting_json) {
    try {
        if (plugin == nullptr || setting_json == nullptr) {
            return -1;
        }
        DriverState* st = as_plugin(plugin)->st;
        const std::string json(setting_json);
        std::string host;
        std::uint64_t port = 0;
        std::uint64_t timeout = 0;
        if (json_get_string(json, "host", host)) {
            st->host = host;
        }
        if (json_get_uint(json, "port", port)) {
            st->port = static_cast<std::uint16_t>(port);
        }
        if (json_get_uint(json, "timeout", timeout)) {
            st->timeout_ms = static_cast<std::uint32_t>(timeout);
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int gw_driver_validate_tag(neu_plugin_t* /*plugin*/, neu_datatag_t* tag) {
    try {
        if (tag == nullptr || tag->address == nullptr) {
            return -1;
        }
        return parse_address(tag->address).ok ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

// ── 周期读 + 上报 ───────────────────────────────────────────────────────────
extern "C" int gw_driver_group_timer(neu_plugin_t* plugin, neu_plugin_group_t* group) {
    try {
        if (plugin == nullptr || group == nullptr || group->tags == nullptr) {
            return -1;
        }
        GwPlugin* p = as_plugin(plugin);
        DriverState* st = p->st;
        if (!st->started) {
            return -1;
        }

        const unsigned n = utarray_len(group->tags);
        int failed = 0;
        for (unsigned i = 0; i < n; ++i) {
            auto* tag = static_cast<neu_datatag_t*>(utarray_eltptr(group->tags, i));
            if (tag == nullptr || tag->name == nullptr) {
                continue;
            }
            const ParsedAddress pa = parse_address(tag->address);
            if (!pa.ok) {
                note_error(st, std::string("点位地址非法: ") + (tag->address ? tag->address : ""));
                ++failed;
                continue;
            }

            std::vector<std::uint16_t> vals;
            if (!st->client.read_holding(pa.reg, 1, vals) || vals.empty()) {
                note_error(st, std::string("读点位失败: ") + tag->name + " (" +
                                   st->client.last_error() + ")");
                ++failed;
                continue;
            }
            ++st->reads;
            ++g_reads;

            // ★ 唯一的对外出口：通过 Neuron 给的回调上报值。
            //   真实驱动的写法就是这一句 —— 所以我们把它的名字与签名抄准了。
            //   注意 update / write_response 在真 SDK 里藏在 union 的 driver
            //   子结构中（P0-3 真头联调实测），必须写 ->driver.update。
            if (p->common.adapter_callbacks != nullptr &&
                p->common.adapter_callbacks->driver.update != nullptr) {
                neu_dvalue_t dv;
                dv.type      = NEU_TYPE_UINT16;   // 真 Neuron 按 type 解释载荷，必须显式给
                dv.value.u16 = vals[0];
                p->common.adapter_callbacks->driver.update(p->common.adapter, group->group_name,
                                                           tag->name, dv);
                ++st->updates;
                ++g_updates;
            }
        }
        return failed == 0 ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

extern "C" int gw_driver_write_tag(neu_plugin_t* plugin, void* req, neu_datatag_t* tag,
                                  neu_value_u value) {
    try {
        if (plugin == nullptr || tag == nullptr) {
            return -1;
        }
        GwPlugin* p = as_plugin(plugin);
        DriverState* st = p->st;
        const ParsedAddress pa = parse_address(tag->address);
        int err = 0;
        if (!pa.ok) {
            err = -1;
        } else if (!st->client.write_single(pa.reg, value.u16)) {
            note_error(st, std::string("写点位失败: ") + tag->name);
            err = -1;
        }
        // 真实 Neuron 要求通过回调把写结果回执给它（连同它传下来的 req）
        if (p->common.adapter_callbacks != nullptr &&
            p->common.adapter_callbacks->driver.write_response != nullptr) {
            p->common.adapter_callbacks->driver.write_response(p->common.adapter, req, err);
        }
        return err;
    } catch (...) {
        return -1;
    }
}

// ── 测试观察口 ─────────────────────────────────────────────────────────────
extern "C" uint64_t gw_driver_stat_reads(void) { return g_reads; }
extern "C" uint64_t gw_driver_stat_updates(void) { return g_updates; }
extern "C" uint64_t gw_driver_stat_errors(void) { return g_errors; }
extern "C" const char* gw_driver_last_error(void) { return g_last_error.c_str(); }
