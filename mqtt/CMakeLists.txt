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
    # 无本地路径 ⇒ 用根 CMakeLists 已 FetchContent 的 v1.3.14 目标
    # （见根文件的 addSocket bug 注释；apt 的 1.3.13 同病不用）
    foreach(cand paho-mqtt3a-static paho-mqtt3a)
        if(TARGET "${cand}")
            set(GW_PAHO_TARGET "${cand}")
            break()
        endif()
    endforeach()
    if(GW_PAHO_TARGET STREQUAL "")
        message(FATAL_ERROR
            "找不到 paho-mqtt3a 目标 —— 根 CMakeLists 的 FetchContent 未生效？"
            "（本地路径方案：-DGW_PAHO_INCLUDE_DIR/-DGW_PAHO_LIBRARY）")
    endif()
    message(STATUS "mqtt: 使用 FetchContent Paho -> ${GW_PAHO_TARGET}")
endif()

add_library(gw_mqtt STATIC src/mqtt_transport.cpp)
# Paho 用 PUBLIC 暴露：gw_mqtt 的使用者（如 mqtt_e2e 的订阅者角色）本来就要直接调 Paho。
# 注意 gw_mqtt 的**头文件**里不含任何 Paho 类型，所以不想碰 Paho 的使用者不受影响。
target_link_libraries(gw_mqtt PUBLIC gw_common ${GW_PAHO_TARGET})
target_include_directories(gw_mqtt PUBLIC include)

# ── 告警发布器（W4-1）：规则引擎的告警 → MQTT JSON 主题 ─────────────────────
# 需要规则引擎（Alert 类型）与 MQTT 同时开启；独立 target：
#   · Alert 类型经 gw_rules 传递，nlohmann（rules/ 已 FetchContent）负责转义
#   · Paho 头经 gw_mqtt PUBLIC 传递；不与 MqttTransport 共享客户端/完成槽
if(TARGET gw_rules AND GW_PAHO_TARGET)
    add_library(gw_alerts STATIC src/alert_publisher.cpp)
    target_link_libraries(gw_alerts
        PUBLIC gw_rules gw_mqtt   # gw_mqtt PUBLIC：让测试/使用者拿到 Paho 头与库
        PRIVATE nlohmann_json::nlohmann_json)
    target_include_directories(gw_alerts PUBLIC include)
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(gw_alerts PRIVATE Threads::Threads)
    endif()
    message(STATUS "alerts: 告警发布器已启用（gw_alerts）")
endif()
