# Neuron 驱动插件（自研件②）
#
# 两种构建路线：
#   ① 真 SDK：-DGW_NEURON_SDK_DIR=<包含 neuron/ 子目录的根>（装好的 SDK 是 /usr/local/include，
#      垫片同形）。注意 sdk-install.sh 的 echo 提示写的是 /usr/local/include/neuron ——
#      那是给官方插件「平铺 include」用的；我们的源码 #include "neuron/plugin.h"，
#      所以必须给上一层。实测踩过（P0-3 联调）。
#   ② 垫片  ：不传该变量 → 用 sdk_shim/neuron/plugin.h ，
#             从而能在本机编译并被动态加载驱动一遍完整生命周期（把代码问题与 SDK 环境问题分开）
if(GW_NEURON_SDK_DIR)
    set(GW_NEURON_INCLUDE "${GW_NEURON_SDK_DIR}")
    message(STATUS "driver: 使用真实 Neuron SDK -> ${GW_NEURON_INCLUDE}")
else()
    set(GW_NEURON_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/sdk_shim")
    message(STATUS "driver: 使用内置 SDK 垫片 -> ${GW_NEURON_INCLUDE}（仅供本机验证）")
endif()

# 真 SDK 的 json 封装头位于 <SDK>/neuron/json/（json-c 被扁平化收进去了），
# 且它们内部以 #include "json/json.h" 互引 —— 只有 <SDK>/neuron 这条 include
# 路径能解析到它。垫片布局没有这层目录，EXISTS 守卫保证两条路线都干净。
if(EXISTS "${GW_NEURON_INCLUDE}/neuron/json")
    set(GW_NEURON_JSON_INCLUDE "${GW_NEURON_INCLUDE}/neuron")
    message(STATUS "driver: 真 SDK json 封装路径 -> ${GW_NEURON_JSON_INCLUDE}")
else()
    set(GW_NEURON_JSON_INCLUDE "")
endif()

# 必须开启 C 语言：模块描述符只能用 C99 的嵌套指定初始化器写（见 plugin_module.c 注释）
enable_language(C)

# 插件本体：C 编译单元（描述符）+ C++ 编译单元（业务逻辑），
# 产出一个**动态库** —— 因为 Neuron 就是 dlopen 它、再 dlsym("neu_plugin_module")。
add_library(gw_driver SHARED
    plugin_module.c
    driver_impl.cpp
)
target_link_libraries(gw_driver PRIVATE gw_client)
# 垫片头放在 SYSTEM 之后也没关系（它是我们自己的、按 -Wall 写的）；真 SDK 时同样安全
target_include_directories(gw_driver PRIVATE "${GW_NEURON_INCLUDE}")
if(GW_NEURON_JSON_INCLUDE)
    target_include_directories(gw_driver PRIVATE "${GW_NEURON_JSON_INCLUDE}")
endif()
target_compile_features(gw_driver PRIVATE cxx_std_17)

if(NOT WIN32)
    # Linux 上插件是 .so，符号默认可见即可；
    # 真实构建时这里还要补 libneuron-base.so 的链接与 rpath（见 README 的 WSL 说明）
endif()

# Windows 的 DLL 不自动导出符号（Linux 的 .so 默认导出，真目标不需要）——
# 本机要动态加载验证，所以打开自动导出。
set_target_properties(gw_driver PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 「假 Neuron」：动态加载插件、驱动完整生命周期
add_executable(driver_check tests/abi_check.cpp)
target_link_libraries(driver_check PRIVATE gw_common)
target_include_directories(driver_check PRIVATE "${GW_NEURON_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}")
if(GW_NEURON_JSON_INCLUDE)
    target_include_directories(driver_check PRIVATE "${GW_NEURON_JSON_INCLUDE}")
endif()
# 依赖插件产物，保证 ctest 时库是新的
add_dependencies(driver_check gw_driver)

# 不指定设备 → 只驱动生命周期与 ABI（可进 CI）；给了 --device-port 才做取值断言
add_test(NAME driver_abi_check COMMAND driver_check --lib $<TARGET_FILE:gw_driver>)
set_tests_properties(driver_abi_check PROPERTIES TIMEOUT 60)
