/*
 * 仅供本机验证的 Neuron SDK 最小垫片。**不要用于生产构建。**
 *
 * 目的：让驱动插件（plugins/driver）能在没有 Linux/Neuron 环境的机器上被真正编译、
 *      并被动态加载驱动一遍完整生命周期 —— 从而把「代码问题」与「SDK 环境问题」分开。
 *      （参见技能 verify-code-before-target-env-via-api-shim）
 *
 * ★ 垫片范围声明（诚实边界，别把"垫片过了"当成"真 SDK 一定能编"）：
 *   · neu_plugin_intf_funs_t / neu_plugin_module_t / neu_datatag_t /
 *     neu_datatag_addr_option_u / neu_plugin_group_t —— **逐字**抄自真实头文件
 *     （plugin.h / tag.h），包括那个关键的 `union { struct {...} driver; }`。
 *     这是本垫片要保真的核心。
 *   · neu_plugin_common_t / adapter_callbacks_t / neu_dvalue_t / neu_value_u ——
 *     真实结构体字段更多，这里只保留我们用到的。**保真的是「字段名与签名」**：
 *     插件里一律按名字访问（plugin->common.adapter_callbacks->update(...)），
 *     因此垫片与真 SDK 都能编过。
 *   · UT_array —— 真实是 uthash 的 utarray；这里提供同名宏 utarray_len/utarray_eltptr，
 *     保真的同样是「用法」层面。
 *
 * 用法：-DGW_NEURON_SDK_DIR=<真 SDK>/include 时走真头文件；否则用本垫片。
 */
#ifndef NEURON_SDK_SHIM_PLUGIN_H
#define NEURON_SDK_SHIM_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── 1. define.h 的长度常量与版本（与真实一致）────────────────────────────── */
#define NEU_NODE_NAME_LEN 64
#define NEU_GROUP_NAME_LEN 64
#define NEU_TAG_NAME_LEN 128
#define NEU_TAG_META_LENGTH 16
#define NEU_TAG_FORMAT_LENGTH 32
#define NEU_VERSION_MAJOR 2
#define NEU_VERSION_MINOR 16
#define NEU_VERSION_FIX 0
#define NEU_VERSION(major, minor, fix) ((major) << 16 | (minor) << 8 | (fix))
#define NEURON_PLUGIN_VER_1_0 NEU_VERSION(2, 16, 0)

/* ── 2. 不透明类型与前置 typedef ─────────────────────────────────────────── */
typedef struct neu_adapter      neu_adapter_t;
typedef struct neu_reqresp_head neu_reqresp_head_t;
typedef struct neu_tag_meta     neu_tag_meta_t;
typedef struct neu_plugin       neu_plugin_t;
typedef struct neu_plugin_group neu_plugin_group_t;
typedef struct adapter_callbacks adapter_callbacks_t;

/* ── 3. 枚举（值取自真实 type.h / tag.h / define.h）───────────────────────── */
typedef enum {
    NEU_TYPE_ERROR  = 0,
    NEU_TYPE_INT8   = 1,
    NEU_TYPE_UINT8  = 2,
    NEU_TYPE_INT16  = 3,
    NEU_TYPE_UINT16 = 4,
    NEU_TYPE_INT32  = 5,
    NEU_TYPE_UINT32 = 6,
    NEU_TYPE_INT64  = 7,
    NEU_TYPE_UINT64 = 8,
    NEU_TYPE_FLOAT  = 9,
    NEU_TYPE_DOUBLE = 10,
    NEU_TYPE_BIT    = 11,
    NEU_TYPE_BOOL   = 12,
    NEU_TYPE_STRING = 13,
    NEU_TYPE_BYTES  = 14,
} neu_type_e;

typedef enum {
    NEU_ATTRIBUTE_READ      = 1,
    NEU_ATTRIBUTE_WRITE     = 2,
    NEU_ATTRIBUTE_SUBSCRIBE = 4,
} neu_attribute_e;

typedef enum {
    NEU_DATATAG_ENDIAN_L16  = 0,
    NEU_DATATAG_ENDIAN_B16  = 1,
    NEU_DATATAG_ENDIAN_LL32 = 2,
    NEU_DATATAG_ENDIAN_LB32 = 3,
    NEU_DATATAG_ENDIAN_BB32 = 4,
    NEU_DATATAG_ENDIAN_BL32 = 5,
    NEU_DATATAG_ENDIAN_L64  = 6,
    NEU_DATATAG_ENDIAN_B64  = 7,
} neu_datatag_endian_e;

typedef enum {
    NEU_DATATAG_STRING_TYPE_H = 0,
    NEU_DATATAG_STRING_TYPE_L = 1,
    NEU_DATATAG_STRING_TYPE_D = 2,
    NEU_DATATAG_STRING_TYPE_E = 3,
} neu_datatag_string_type_e;

typedef enum {
    NEU_NA_TYPE_DRIVER = 0,
    NEU_NA_TYPE_APP    = 1,
} neu_node_type_e;

typedef enum {
    NEU_PLUGIN_KIND_SYSTEM = 0,
    NEU_PLUGIN_KIND_CUSTOM = 1,
} neu_plugin_kind_e;

typedef enum {
    NEU_EVENT_TIMER_NO     = 0,
    NEU_EVENT_TIMER_ALWAYS = 1,
} neu_event_timer_type_e;

typedef enum {
    NEU_TAG_CACHE_TYPE_NONE = 0,
} neu_tag_cache_type_e;

/* ── 4. 值类型与上报回调 ────────────────────────────────────────────────── */
/* 真实 neu_value_u 更长（含位域结构体等）；这里保留标量成员，够插件读写用 */
typedef union neu_value {
    uint8_t  u8;
    int8_t   i8;
    uint16_t u16;
    int16_t  i16;
    uint32_t u32;
    int32_t  i32;
    uint64_t u64;
    int64_t  i64;
    float    f32;
    double   f64;
    uint8_t  bytes[8];
} neu_value_u;

/* 真实 neu_dvalue_t 字段更多（含 type/精度等）；这里保留 .value */
typedef struct {
    neu_value_u value;
} neu_dvalue_t;

struct adapter_callbacks {
    /* 真实结构体更长（update_with_trace / update_im / ...），我们只用这两个。
       保真的是**字段名与签名**（从左到右抄自真实 adapter.h）。 */
    void (*update)(neu_adapter_t *adapter, const char *group, const char *tag,
                   neu_dvalue_t value);
    void (*write_response)(neu_adapter_t *adapter, void *req, int error);
};

/* ── 5. 点位（neu_datatag_addr_option_u 与 neu_datatag_t 逐字抄）────────── */
typedef union {
    struct {
        neu_datatag_endian_e endian;
    } value16;
    struct {
        neu_datatag_endian_e endian;
        bool                 is_default;
    } value32;
    struct {
        neu_datatag_endian_e endian;
        bool                 is_default;
    } value64;
    struct {
        uint16_t                  length;
        neu_datatag_string_type_e type;
        bool                      is_default;
    } string;
    struct {
        uint8_t length;
    } bytes;
    struct {
        bool    op;
        uint8_t bit;
    } bit;
} neu_datatag_addr_option_u;

typedef struct {
    char *                    name;
    char *                    address;
    neu_attribute_e           attribute;
    neu_type_e                type;
    uint8_t                   precision;
    double                    decimal;
    double                    bias;
    char *                    description;
    char *                    unit;
    neu_datatag_addr_option_u option;
    uint8_t                   meta[NEU_TAG_META_LENGTH];
    uint8_t                   format[NEU_TAG_FORMAT_LENGTH];
    uint8_t                   n_format;
} neu_datatag_t;

/* ── 6. UT_array（真实是 uthash 的 utarray；保真到"用法"层面）──────────── */
typedef struct {
    void * data;
    size_t elem_size;
    size_t len;
    size_t cap;
} UT_array;

#define utarray_len(a) ((unsigned) (((const UT_array *) (a))->len))
#define utarray_eltptr(a, i)                                                       \
    ((void *) (((char *) ((const UT_array *) (a))->data) +                         \
               ((size_t) (i)) * ((const UT_array *) (a))->elem_size))

/* ── 7. 插件接口表与模块描述符（逐字抄 plugin.h）────────────────────────── */
typedef void (*neu_plugin_group_free)(neu_plugin_group_t *pgp);

struct neu_plugin_group {
    char *    group_name;
    UT_array *tags;

    void *                context;
    void *                user_data;
    neu_plugin_group_free group_free;
    uint32_t              interval;
};

typedef int (*neu_plugin_tag_validator_t)(const neu_datatag_t *tag);

typedef struct {
    neu_datatag_t *tag;
    neu_value_u    value;
    neu_type_e     type;
} neu_plugin_tag_value_t;

typedef struct neu_plugin_intf_funs {
    neu_plugin_t *(*open)(void);
    int (*close)(neu_plugin_t *plugin);
    int (*init)(neu_plugin_t *plugin, bool load);
    int (*uninit)(neu_plugin_t *plugin);
    int (*start)(neu_plugin_t *plugin);
    int (*stop)(neu_plugin_t *plugin);
    int (*setting)(neu_plugin_t *plugin, const char *setting);
    int (*try_connect)(neu_plugin_t *plugin);

    int (*request)(neu_plugin_t *plugin, neu_reqresp_head_t *head, void *data);

    union {
        struct {
            int (*validate_tag)(neu_plugin_t *plugin, neu_datatag_t *tag);
            int (*group_timer)(neu_plugin_t *plugin, neu_plugin_group_t *group);
            int (*group_sync)(neu_plugin_t *plugin, neu_plugin_group_t *group);
            int (*write_tag)(neu_plugin_t *plugin, void *req, neu_datatag_t *tag,
                             neu_value_u value);
            int (*write_tags)(neu_plugin_t *plugin, void *req, UT_array *tag_values);
            neu_plugin_tag_validator_t tag_validator;

            int (*load_tags)(neu_plugin_t *plugin, const char *group,
                             neu_datatag_t *tags, int n_tag);
            int (*add_tags)(neu_plugin_t *plugin, const char *group,
                            neu_datatag_t *tags, int n_tag);
            int (*del_tags)(neu_plugin_t *plugin, int n_tag);
            int (*scan_tags)(neu_plugin_t *plugin, void *req, char *id, char *ctx,
                             int load_index);
            int (*test_read_tag)(neu_plugin_t *plugin, void *req, neu_datatag_t tag);
            int (*action)(neu_plugin_t *plugin, const char *action);

            int (*directory)(neu_plugin_t *plugin, void *req, const char *path);
            int (*fup_open)(neu_plugin_t *plugin, void *req, const char *path);
            int (*fup_data)(neu_plugin_t *plugin, void *req, const char *path);
            int (*fdown_open)(neu_plugin_t *plugin, void *req, const char *node,
                              const char *src_path, const char *dst_path, int64_t size);
            int (*fdown_data)(neu_plugin_t *plugin, void *req, uint8_t *bytes,
                              uint16_t n_bytes, bool more);
        } driver;
    };
} neu_plugin_intf_funs_t;

typedef struct neu_plugin_module {
    const uint32_t                version;
    const char *                  schema;
    const char *                  module_name;
    const char *                  module_descr;
    const char *                  module_descr_zh;
    const neu_plugin_intf_funs_t *intf_funs;
    neu_node_type_e               type;
    neu_plugin_kind_e             kind;
    bool                          display;
    bool                          single;
    const char *                  single_name;
    neu_event_timer_type_e        timer_type;
    neu_tag_cache_type_e          cache_type;
} neu_plugin_module_t;

/* ── 8. 插件公共头（真实字段更多，这里保留我们用到的）─────────────────────
 * 注意：**不在这里定义 struct neu_plugin** —— 真实 SDK 里 neu_plugin 只有前置声明，
 * 由各插件自己定义（第一个成员必须是 neu_plugin_common_t）。垫片保持同样的规矩，
 * 所以插件侧与「假 Neuron」自检侧可以各自定义自己的版本（只共享首成员布局）。 */
typedef struct neu_plugin_common {
    uint32_t                   magic;
    neu_adapter_t *            adapter;
    const adapter_callbacks_t *adapter_callbacks;
    char                       name[NEU_NODE_NAME_LEN];
} neu_plugin_common_t;

#endif /* NEURON_SDK_SHIM_PLUGIN_H */
