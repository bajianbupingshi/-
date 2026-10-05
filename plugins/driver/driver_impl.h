/*
 * 驱动插件对 C 层暴露的接口。
 *
 * ★ 这个头文件是「C/C++ ABI 边界」的契约，两条硬规则：
 *   1) 全部函数用 extern "C" 导出 —— 名字不被 C++ mangle，C 编译单元才能引用。
 *   2) **任何 C++ 异常都不许穿越这个边界**。实现里每个函数都是 try/catch(...) 全包，
 *      失败转成错误码返回。理由：跨语言边界的异常没有约定，行为未定义；
 *      而且上层（Neuron 内核）是 C，它没有 catch 的概念。
 */
#ifndef GW_DRIVER_IMPL_H
#define GW_DRIVER_IMPL_H

#include <stdbool.h>
#include <stdint.h>

#include "neuron/plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Neuron 插件生命周期 ────────────────────────────────────────────────── */
neu_plugin_t *gw_driver_open(void);
int gw_driver_close(neu_plugin_t *plugin);
int gw_driver_init(neu_plugin_t *plugin, bool load);
int gw_driver_uninit(neu_plugin_t *plugin);
int gw_driver_start(neu_plugin_t *plugin);
int gw_driver_stop(neu_plugin_t *plugin);
int gw_driver_setting(neu_plugin_t *plugin, const char *setting_json);

/* ── 驱动语义 ──────────────────────────────────────────────────────────── */
/* 返回 0 表示该点位可被本驱动接受 */
int gw_driver_validate_tag(neu_plugin_t *plugin, neu_datatag_t *tag);
/* 周期回调：读该组所有点位并通过 adapter_callbacks->update() 上报 */
int gw_driver_group_timer(neu_plugin_t *plugin, neu_plugin_group_t *group);
/* 写单个点位；通过 adapter_callbacks->write_response() 回执 */
int gw_driver_write_tag(neu_plugin_t *plugin, void *req, neu_datatag_t *tag,
                        neu_value_u value);

/* ── 仅供测试观察（不属于 Neuron 接口）─────────────────────────────────── */
uint64_t gw_driver_stat_reads(void);
uint64_t gw_driver_stat_updates(void);
uint64_t gw_driver_stat_errors(void);
const char *gw_driver_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* GW_DRIVER_IMPL_H */
