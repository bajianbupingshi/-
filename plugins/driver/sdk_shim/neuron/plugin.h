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
 *     真实结构体字段更多，这里只保留我们用到的。**保真的是「字段名、签名与嵌套形状」**：
 *     插件里一律按名字访问（plugin->common.adapter_callbacks->driver.update(...)），
 *     因此垫片与真 SDK 都能编过（update / write_response 藏在 union 的
 *     driver 子结构里 —— 这个形状差异是真头联调时踩出来的，见 README）。
 *   · UT_array —— 逐字对齐 uthash 2.3.0（d / icd.sz / i / n），连内部布局一起保真，
 *     手工构造数组的测试代码才不必为两条路线写两份。
 *
 * 用法：-DGW_NEURON_SDK_DIR=<真 SDK>/include 时走真头文件；否则用本垫片。
 */
#ifndef NEURON_SDK_SHIM_PLUGIN_H
#define NEURON_SDK_SHIM_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── 1. define.h 的长度常量与版本（逐字对齐本地 main-daily 真头，
      P0-3 联调时由 tools/check_sdk_shim.py 抓出 6 处漂移后修正）────────────── */
#define NEU_NODE_NAME_LEN 128
#define NEU_GROUP_NAME_LEN 128
#define NEU_TAG_NAME_LEN 128
#define NEU_TAG_META_LENGTH 20
#define NEU_TAG_FORMAT_LENGTH 16
#define NEU_VERSION_MAJOR 2
#define NEU_VERSION_MINOR 15
#define NEU_VERSION_FIX 0
#define NEU_VERSION(major, minor, fix) ((major) << 16 | (minor) << 8 | (fix))
#define NEURON_PLUGIN_VER_1_0 NEU_VERSION(2, 15, 0)

/* ── 2. 不透明类型与前置 typedef ─────────────────────────────────────────── */
typedef struct neu_adapter      neu_adapter_t;
typedef struct neu_reqresp_head neu_reqresp_head_t;
typedef struct neu_tag_meta     neu_tag_meta_t;
typedef struct neu_plugin       neu_plugin_t;
typedef struct neu_plugin_group neu_plugin_group_t;
typedef struct adapter_callbacks adapter_callbacks_t;

/* ── 3. 枚举（值取自真实 type.h / tag.h / define.h。
      注意：main-daily 的 type 枚举已把 ERROR 挪到 15（前面 1~14 是
      INT8..BYTES），不再是旧版的 0 —— P0-3 漂移复核抓出来的）────────────── */
typedef enum {
    NEU_TYPE_ERROR  = 15,
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
    NEU_NA_TYPE_DRIVER = 1,
    NEU_NA_TYPE_APP    = 2,
} neu_adapter_type_e,
    neu_node_type_e;

/* ★ 取值必须与真实 define.h 一致（曾经写错成 0/1，见文件尾的垫片漂移检查） */
typedef enum neu_plugin_kind {
    NEU_PLUGIN_KIND_STATIC = 0,
    NEU_PLUGIN_KIND_SYSTEM = 1,
    NEU_PLUGIN_KIND_CUSTOM = 2,
} neu_plugin_kind_e;

/* 真实定义在 event/event.h：BLOCK=0 / NOBLOCK=1（不是 NO/ALWAYS —— 曾写错） */
typedef enum {
    NEU_EVENT_TIMER_BLOCK   = 0,
    NEU_EVENT_TIMER_NOBLOCK = 1,
} neu_event_timer_type_e;

/* 真实定义里没有 _NONE（曾凭想象发明过一个常量） */
typedef enum {
    NEU_TAG_CACHE_TYPE_INTERVAL = 0,
    NEU_TAG_CACHE_TYPE_NEVER    = 1,
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

/* 逐字对齐真实 neu_dvalue_t（type.h）—— P0-3 联调发现插件必须显式给
   dv.type（真 Neuron 按它解释载荷），垫片结构里没有 type 就编不过 */
typedef struct {
    neu_type_e  type;
    neu_value_u value;
    uint8_t     precision;
} neu_dvalue_t;

struct adapter_callbacks {
    /* 真实结构体更长（command / response / responseto / register_metric /
       update_metric / update_with_trace / update_im / ...）。
       ★ P0-3 真头联调实测的形状差异：update / write_response 不在顶层，
       藏在 union { struct {...} driver; } 里 —— 访问必须写
       ->driver.update / ->driver.write_response（探针验证过，写 ->update
       直接 "has no member named 'update'"）。这里保真到嵌套形状，
       字段仍只留用到的两个。 */
    union {
        struct {
            void (*update)(neu_adapter_t *adapter, const char *group, const char *tag,
                           neu_dvalue_t value);
            void (*write_response)(neu_adapter_t *adapter, void *req, int error);
        } driver;
    };
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

/* ── 6. UT_array（逐字抄 uthash 2.3.0 的 utarray.h。
      P0-3 真头联调实测：内部字段名（d / icd.sz / i / n）若与真实不一致，
      「手工构造数组」的测试代码就会两路漂移 —— 所以连内部布局一起保真，
      测试代码得以用同一份字段名走垫片与真 SDK 两条路线）────────────── */
typedef void (ctor_f)(void *dst, const void *src);
typedef void (dtor_f)(void *elt);
typedef void (init_f)(void *elt);
typedef struct {
    size_t  sz;
    init_f *init;
    ctor_f *copy;
    dtor_f *dtor;
} UT_icd;

typedef struct {
    unsigned i, n; /* i: index of next available slot, n: num slots */
    UT_icd   icd;  /* initializer, copy and destructor functions */
    char    *d;    /* n slots of size icd.sz */
} UT_array;

#define utarray_len(a) ((a)->i)
#define utarray_eltptr(a, j) (((j) < (a)->i) ? _utarray_eltptr(a, j) : NULL)
#define _utarray_eltptr(a, j) ((void *) ((a)->d + ((a)->icd.sz * (j))))

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

/* open() 里必须把 magic 设为 NEU_PLUGIN_MAGIC_NUMBER（0x43474D50，即 "PMGC"）——
 * Neuron 建节点时 neu_plugin_common_check() 只认它。P0-3 真机联调实测：
 * magic 为 0 会在 neu_adapter_create 的 assert 处把整个 Neuron 进程打崩。
 * 官方的 neu_plugin_common_init() 是宿主符号（真 SDK 头不导出 MAGIC 宏），
 * 本插件改为直接赋值，让 .so 保持零外部符号依赖。 */

#endif /* NEURON_SDK_SHIM_PLUGIN_H */
