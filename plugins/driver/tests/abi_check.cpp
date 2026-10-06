// 驱动插件的 ABI / 生命周期自检 —— 程序本身扮演「最小 Neuron」。
//
// 它做的事就是 Neuron 内核真正会做的事：
//   dlopen("libgw_driver.so") → dlsym("neu_plugin_module") → 读描述符
//   → 调 intf_funs->open() → 塞 adapter/adapter_callbacks → init → setting
//   → validate_tag → start → driver.group_timer（周期回调）→ write_tag → stop → close
//
// 为什么值得单独做：这一层跨越了三个边界，任一处错了都只在运行期暴露 ——
//   ① 动态库符号导出（dlsym 能不能拿到 neu_plugin_module）
//   ② C 结构体布局（我们按 .driver.validate_tag 填的接口表，C 侧读到的对不对）
//   ③ C++ 异常不许穿越 extern "C"（畸形输入下进程不能崩）
// 用真 Neuron 验证需要整套 WSL 环境；用这个「假 Neuron」在任意机器上都能跑。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "neuron/plugin.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// 与插件侧一致：neu_plugin 的第一个成员是 neu_plugin_common_t。
// （真实 Neuron 里 neu_plugin 也是各插件自己定义的；这里只需要访问 common。）
struct neu_plugin {
    neu_plugin_common_t common;
};

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("    [FAIL] %s\n", what.c_str());
    }
}

struct Reported {
    std::string group;
    std::string tag;
    std::uint16_t value = 0;
};
std::vector<Reported> g_reported;
bool g_write_response_seen = false;
int g_write_response_error = -1;

// ── Neuron 会提供的回调（由本程序扮演）────────────────────────────────────
void on_update(neu_adapter_t* /*adapter*/, const char* group, const char* tag,
               neu_dvalue_t value) {
    Reported r;
    r.group = (group != nullptr) ? group : "";
    r.tag = (tag != nullptr) ? tag : "";
    r.value = value.value.u16;
    g_reported.push_back(r);
}

void on_write_response(neu_adapter_t* /*adapter*/, void* /*req*/, int error) {
    g_write_response_seen = true;
    g_write_response_error = error;
}

// ── 极简动态库加载 ────────────────────────────────────────────────────────
#if defined(_WIN32)
using lib_handle = HMODULE;
lib_handle load_lib(const char* path) { return ::LoadLibraryA(path); }
void* load_sym(lib_handle h, const char* name) {
    return reinterpret_cast<void*>(::GetProcAddress(h, name));
}
void close_lib(lib_handle h) { ::FreeLibrary(h); }
const char* lib_error() { return "LoadLibrary/GetProcAddress 失败"; }
#else
using lib_handle = void*;
lib_handle load_lib(const char* path) { return ::dlopen(path, RTLD_NOW); }
void* load_sym(lib_handle h, const char* name) { return ::dlsym(h, name); }
void close_lib(lib_handle h) { ::dlclose(h); }
const char* lib_error() { return ::dlerror(); }
#endif


}  // namespace

int main(int argc, char** argv) {
    std::string lib_path;
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;   // 0 = 不做设备相关断言，只驱动生命周期

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--lib") {
            lib_path = val();
        } else if (a == "--device-host") {
            host = val();
        } else if (a == "--device-port") {
            port = static_cast<std::uint16_t>(std::strtoul(val(), nullptr, 10));
        } else if (a == "--help" || a == "-h") {
            std::printf("用法: driver_check --lib <插件动态库路径> [--device-host H --device-port P]\n");
            return 0;
        }
    }
    if (lib_path.empty()) {
        std::printf("必须指定 --lib <插件动态库路径>\n");
        return 2;
    }

    std::printf("=========== 驱动插件 ABI / 生命周期自检 ===========\n");
    std::printf("  插件: %s\n", lib_path.c_str());
    std::printf("  设备: %s\n", port == 0 ? "(未指定，跳过取值断言)" : (host + ":" +
                                                                  std::to_string(port)).c_str());

    // ① 动态加载 + 取模块描述符
    lib_handle lib = load_lib(lib_path.c_str());
    check(lib != nullptr, "① 动态库应能加载");
    if (lib == nullptr) {
        std::printf("    %s\n", lib_error());
        std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
        return 1;
    }

    auto* module = static_cast<neu_plugin_module_t*>(load_sym(lib, "neu_plugin_module"));
    check(module != nullptr, "① 应能取到符号 neu_plugin_module（Neuron 就是 dlsym 它）");
    if (module == nullptr) {
        close_lib(lib);
        std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
        return 1;
    }

    // ② 描述符内容（远端读的是同一块内存，跨编译单元的布局必须一致）
    std::printf("  描述符: name=\"%s\" type=%d kind=%d timer=%d\n",
                module->module_name != nullptr ? module->module_name : "(null)",
                static_cast<int>(module->type), static_cast<int>(module->kind),
                static_cast<int>(module->timer_type));
    check(module->module_name != nullptr &&
              std::strcmp(module->module_name, "NGWP Sim") == 0,
          "② module_name 应为 NGWP Sim");
    // ★ 用**字面量**断言，不能用垫片里的常量：两侧同错的话自检照样全绿。
    //   实测踩过 —— 垫片曾把 DRIVER 写成 0、SYSTEM 写成 0，22/22 通过但对真 SDK 是错的。
    check(module->type == 1, "② type 必须等于真实 SDK 的 NEU_NA_TYPE_DRIVER=1");
    check(module->kind == 1, "② kind 必须等于真实 SDK 的 NEU_PLUGIN_KIND_SYSTEM=1");
    check(module->intf_funs != nullptr, "② intf_funs 应非空");

    const neu_plugin_intf_funs_t* funs = module->intf_funs;
    check(funs != nullptr && funs->open != nullptr, "② open 回调应存在");
    // ★ 关键：C 侧能否正确读到嵌套 struct 里的函数指针
    check(funs != nullptr && funs->driver.group_timer != nullptr,
          "② ★ 嵌套 .driver.group_timer 应非空（C 结构体布局正确）");
    check(funs != nullptr && funs->driver.validate_tag != nullptr, "② ★ .driver.validate_tag 应非空");
    check(funs != nullptr && funs->driver.write_tag != nullptr, "② ★ .driver.write_tag 应非空");

    // ③ 生命周期
    neu_plugin_t* plugin = funs->open();
    check(plugin != nullptr, "③ open() 应返回非空插件实例");
    if (plugin == nullptr) {
        close_lib(lib);
        std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
        return 1;
    }

    adapter_callbacks_t cbs;
    std::memset(&cbs, 0, sizeof(cbs));
    // update / write_response 在真 SDK 里藏在 union 的 driver 子结构中
    //（P0-3 真头联调实测，写 ->update 直接 "has no member named 'update'"）
    cbs.driver.update = on_update;
    cbs.driver.write_response = on_write_response;
    plugin->common.adapter = nullptr;   // 假 Neuron：adapter 对本插件不重要
    plugin->common.adapter_callbacks = &cbs;

    check(funs->init(plugin, false) == 0, "③ init() 应成功");

    const std::string settings = "{\"host\":\"" + host + "\",\"port\":" +
                                 std::to_string(port == 0 ? 1502 : port) +
                                 ",\"timeout\":1000}";
    check(funs->setting(plugin, settings.c_str()) == 0, "③ setting() 应成功");

    // ④ 点位校验：合法地址通过、非法地址被拒
    neu_datatag_t good{};
    good.name = const_cast<char*>("tag1");
    good.address = const_cast<char*>("1!40001");
    good.attribute = NEU_ATTRIBUTE_READ;
    good.type = NEU_TYPE_UINT16;
    check(funs->driver.validate_tag(plugin, &good) == 0, "④ 合法地址 1!40001 应通过校验");

    neu_datatag_t bad{};
    bad.name = const_cast<char*>("badtag");
    bad.address = const_cast<char*>("not-a-number");
    check(funs->driver.validate_tag(plugin, &bad) != 0, "④ 非法地址应被拒绝");

    // ⑤ 生命周期：start / group_timer / write_tag / stop
    const int start_rc = funs->start(plugin);
    if (port == 0) {
        // 没给设备：start 允许失败，但**绝不能崩**
        std::printf("  ⑤ start() 返回 %d（未指定设备，允许失败）\n", start_rc);
        check(true, "⑤ 设备不可达时应优雅失败而不是崩溃");
    } else {
        check(start_rc == 0, "⑤ start() 应成功连上设备");

        // 搭一个组，含 4 个连续点位
        const unsigned n_tag = 4;
        std::vector<neu_datatag_t> tags(n_tag);
        std::vector<std::string> names = {"t0", "t1", "t2", "t3"};
        std::vector<std::string> addrs = {"1!40001", "1!40002", "1!40003", "1!40004"};
        // 直接构造 utarray 内部布局（字段名与 uthash 2.3.0 一致，垫片已同形）：
        // i = 元素数，n = 容量，icd.sz = 单元素大小，d = 数据首地址
        UT_array tags_arr;
        tags_arr.d = reinterpret_cast<char*>(tags.data());
        tags_arr.icd.sz = sizeof(neu_datatag_t);
        tags_arr.i = n_tag;
        tags_arr.n = n_tag;
        for (unsigned i = 0; i < n_tag; ++i) {
            tags[i] = neu_datatag_t{};
            tags[i].name = const_cast<char*>(names[i].c_str());
            tags[i].address = const_cast<char*>(addrs[i].c_str());
            tags[i].attribute = NEU_ATTRIBUTE_READ;
            tags[i].type = NEU_TYPE_UINT16;
        }

        neu_plugin_group_t group{};
        group.group_name = const_cast<char*>("group1");
        group.tags = &tags_arr;
        group.interval = 1000;

        g_reported.clear();
        const int timer_rc = funs->driver.group_timer(plugin, &group);
        std::printf("  ⑤ group_timer 返回 %d，上报 %zu 条\n", timer_rc, g_reported.size());
        check(timer_rc == 0, "⑤ group_timer 应成功");
        check(g_reported.size() == static_cast<std::size_t>(n_tag),
              "⑤ ★ 上报条数应等于点位数（回调真的被调到）");
        check(!g_reported.empty() && g_reported[0].tag == "t0", "⑤ 上报的点位名应为 t0");
        check(!g_reported.empty() && g_reported[0].group == "group1", "⑤ 上报的组名应为 group1");
        // 兼容旧口径：上报的值应来自设备（连续读到的 4 个值）
        std::printf("  ⑤ 上报值: ");
        for (const Reported& r : g_reported) {
            std::printf("%s=%u ", r.tag.c_str(), r.value);
        }
        std::printf("\n");

        // ⑥ 写点位 + 回执
        neu_value_u v{};
        v.u16 = 0x1234;
        g_write_response_seen = false;
        const int wr = funs->driver.write_tag(plugin, nullptr, &tags[0], v);
        check(wr == 0, "⑥ write_tag 应成功");
        check(g_write_response_seen, "⑥ ★ 应通过 write_response 回调给出回执");
        check(g_write_response_error == 0, "⑥ 回执错误码应为 0");

        g_reported.clear();
        funs->driver.group_timer(plugin, &group);
        check(!g_reported.empty() && g_reported[0].value == 0x1234,
              "⑥ 写后读：上报值应为刚写入的 0x1234");
    }

    // ⑦ 关闭路径 + 畸形输入不崩（C++ 异常不得穿越 extern "C"）
    check(funs->driver.group_timer(plugin, nullptr) != 0, "⑦ group_timer(NULL) 应返回错误而不崩溃");
    check(funs->setting(plugin, nullptr) != 0, "⑦ setting(NULL) 应返回错误而不崩溃");
    check(funs->driver.validate_tag(plugin, nullptr) != 0, "⑦ validate_tag(NULL) 应返回错误而不崩溃");
    check(funs->stop(plugin) == 0, "⑦ stop() 应成功");
    check(funs->uninit(plugin) == 0, "⑦ uninit() 应成功");
    check(funs->close(plugin) == 0, "⑦ close() 应成功");

    close_lib(lib);
    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
