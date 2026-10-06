/*
 * NGWP 驱动插件的 **C 编译单元** —— 全文只做一件事：定义模块描述符与接口表。
 *
 * ★ 为什么这个文件必须是 .c 而不是 .cpp（实测结论，不是风格偏好）：
 *
 *   neu_plugin_intf_funs_t 内部是
 *       union { struct { ... } driver; };
 *   官方 SDK 示例的写法是
 *       .driver = { .validate_tag = ..., .group_timer = ... }
 *   这是**嵌套指定初始化器**：
 *     · C99 合法；
 *     · **C++20 明确禁止嵌套**（dcl.init.aggr：designated-initializer-list shall not be nested），
 *       而且 C++ 还要求 designator 顺序与声明顺序一致、不允许跳过成员。
 *   再加上 neu_plugin_module_t 的成员带 const（无法「先默认构造再赋值」），
 *   所以「纯 C++ 写插件」会在填描述符这一步直接编译失败。
 *
 *   ⇒ 结论：插件**必须**保留一层 C 编译单元。这不是妥协，正好是简历里那个
 *     「C++ 对象如何跨动态库 ABI 边界」的实物证据。
 */
#include "driver_impl.h"
#include "neuron/plugin.h"

/* Neuron 用 dlsym/GetProcAddress 找的就是 neu_plugin_module 这一个符号。
 * Linux 的 .so 默认导出所有非 static 符号，所以真目标上不需要修饰；
 * Windows 的 DLL 必须显式 dllexport —— 本机动态加载验证才需要它。 */
#if defined(_WIN32)
#define GW_EXPORT __declspec(dllexport)
#else
#define GW_EXPORT __attribute__((visibility("default")))
#endif

/* ── 接口表 ────────────────────────────────────────────────────────────────
 * 注意下面 `.driver = { ... }` 这一层嵌套：它只在这里能写出来。
 * 若要用 C++ 写，只能改成：
 *     static neu_plugin_intf_funs_t g_funs{};          // 先清零
 *     g_funs.driver.validate_tag = ...;                // 再逐字段赋值
 * 代价是描述符不再能是 const、也不再是编译期常量 —— 而 Neuron 期望它是。
 */
static const neu_plugin_intf_funs_t gw_plugin_intf_funs = {
    .open      = gw_driver_open,
    .close     = gw_driver_close,
    .init      = gw_driver_init,
    .uninit    = gw_driver_uninit,
    .start     = gw_driver_start,
    .stop      = gw_driver_stop,
    .setting   = gw_driver_setting,
    .request   = NULL,
    .driver    = {
        .validate_tag = gw_driver_validate_tag,
        .group_timer  = gw_driver_group_timer,
        .write_tag    = gw_driver_write_tag,
    },
};

/* ── 模块描述符：Neuron 通过 dlsym("neu_plugin_module") 取它 ─────────────── */
GW_EXPORT const neu_plugin_module_t neu_plugin_module = {
    .version         = NEURON_PLUGIN_VER_1_0,
    .schema          = "ngwp-sim",   /* 官方写法：不带 .json 扩展名 */
    .module_name     = "NGWP Sim",
    .module_descr    = "NGWP protocol device driver (custom)",
    .module_descr_zh = "NGWP 协议设备驱动（自研）",
    .intf_funs       = &gw_plugin_intf_funs,
    .type            = NEU_NA_TYPE_DRIVER,
    .kind            = NEU_PLUGIN_KIND_SYSTEM,
    .display         = true,
    .single          = false,
    /* 官方 modbus 插件的描述符只写到这里 —— timer_type / cache_type / single_name
       一律省略（designated initializer 里省略即 0）。照着写最安全：
       既不会用错常量，也不会因为某个常量在真 SDK 里不存在而编译失败。 */
};
