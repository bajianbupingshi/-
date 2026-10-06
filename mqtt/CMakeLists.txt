# 真实 MQTT 传输（Paho MQTT C **Async** API）—— 隔离成独立 target，
# 让 gw_common 保持零外部依赖。
#
# Paho 的获取方式（二选一）：
#   ① 本地已有：-DGW_PAHO_INCLUDE_DIR=<paho>/src -DGW_PAHO_LIBRARY=<...>/libpaho-mqtt3a-static.a
#   ② 系统安装：apt install libpaho-mqtt-dev（Ubuntu 24.04 有，3c 同步 / 3a 异步都含）
#
# ★ 必须是 **3a（Async）** 库：传输层 2026-10-06 从 MQTTClient 同步 API 改到
#   MQTTAsync（R9：同步 waitForCompletion 内部 100ms 轮询是单条 ~116ms 的硬地板）。
set(GW_PAHO_TARGET "")

if(GW_PAHO_INCLUDE_DIR AND GW_PAHO_LIBRARY)
    if(NOT EXISTS "${GW_PAHO_INCLUDE_DIR}/MQTTAsync.h")
        message(FATAL_ERROR "GW_PAHO_INCLUDE_DIR 下找不到 MQTTAsync.h：${GW_PAHO_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS "${GW_PAHO_LIBRARY}")
        message(FATAL_ERROR "GW_PAHO_LIBRARY 不存在：${GW_PAHO_LIBRARY}")
    endif()
    add_library(gw_paho INTERFACE)
    # SYSTEM：Paho 的头在 GCC 16 的 -Wall 下会有一堆告警，而本项目全局 -Werror
    target_include_directories(gw_paho SYSTEM INTERFACE "${GW_PAHO_INCLUDE_DIR}")
    target_link_libraries(gw_paho INTERFACE "${GW_PAHO_LIBRARY}")
    if(WIN32)
        # Paho 自己的 CMakeLists 里 Windows 需要这几个系统库
        target_link_libraries(gw_paho INTERFACE ws2_32 crypt32 RpcRT4)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(gw_paho INTERFACE Threads::Threads)
    endif()
    set(GW_PAHO_TARGET gw_paho)
    message(STATUS "mqtt: 使用本地 Paho -> ${GW_PAHO_LIBRARY}")
else()
    find_package(eclipse-paho-mqtt-c CONFIG QUIET)
    # 异步库优先（3a）；传输层改 Async 后同步库（3c）不再可用
    foreach(cand paho-mqtt3a-static paho-mqtt3a eclipse-paho-mqtt-c::paho-mqtt3a-static
                 eclipse-paho-mqtt-c::paho-mqtt3a)
        if(TARGET "${cand}")
            set(GW_PAHO_TARGET "${cand}")
            break()
        endif()
    endforeach()
    if(GW_PAHO_TARGET STREQUAL "")
        message(FATAL_ERROR
            "找不到 Paho MQTT C **Async** 库（paho-mqtt3a）。二选一：\n"
            "  ① 本地：-DGW_PAHO_INCLUDE_DIR=<paho>/src -DGW_PAHO_LIBRARY=<paho>/build/src/libpaho-mqtt3a-static.a\n"
            "  ② 系统：sudo apt install libpaho-mqtt-dev（含 libpaho-mqtt3a）\n"
            "     （注意：用 GCC 16 这类默认 C23 的编译器从源码编 Paho 时，\n"
            "       需要 -DCMAKE_C_STANDARD=99 -DCMAKE_C_FLAGS=-std=gnu99，\n"
            "       否则 MQTTPacket.h 里的 typedef bool 与 C23 关键字冲突；\n"
            "       若 CMake >= 4 还需 -DCMAKE_POLICY_VERSION_MINIMUM=3.5）")
    endif()
    message(STATUS "mqtt: 使用系统 Paho -> ${GW_PAHO_TARGET}")
endif()

add_library(gw_mqtt STATIC src/mqtt_transport.cpp)
# Paho 用 PUBLIC 暴露：gw_mqtt 的使用者（如 mqtt_e2e 的订阅者角色）本来就要直接调 Paho。
# 注意 gw_mqtt 的**头文件**里不含任何 Paho 类型，所以不想碰 Paho 的使用者不受影响。
target_link_libraries(gw_mqtt PUBLIC gw_common ${GW_PAHO_TARGET})
target_include_directories(gw_mqtt PUBLIC include)
