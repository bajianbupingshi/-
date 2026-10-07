cmake_minimum_required(VERSION 3.16)
project(industrial_gateway VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 选项
# ---------------------------------------------------------------------------
option(GW_BUILD_TESTS "构建单元测试"                      ON)
option(GW_BUILD_DEMO  "构建协议层演示程序"                ON)
option(GW_BUILD_PROXY "构建边缘数据代理场景复现器"        ON)
option(GW_BUILD_SIM   "构建 asio 设备模拟器（需要 asio 头文件）" OFF)
option(GW_WITH_MQTT   "构建真实 MQTT 传输与端到端程序（需要 Paho MQTT C + asio）" OFF)
option(GW_FETCH_DEPS  "通过 FetchContent 拉取 spdlog/GoogleTest" OFF)
option(GW_WERROR      "把编译告警视为错误"                ON)
set(GW_ASIO_INCLUDE_DIR "" CACHE PATH "本地 asio 头文件目录（留空则用 FetchContent 拉取）")
set(GW_PAHO_INCLUDE_DIR "" CACHE PATH "本地 Paho MQTT C 头文件目录（其 src/ 目录）")
set(GW_NEURON_SDK_DIR "" CACHE PATH "真实 Neuron SDK 的 include 目录（留空则用内置垫片）")
set(GW_PAHO_LIBRARY     "" CACHE FILEPATH "本地 Paho MQTT C 静态库（libpaho-mqtt3c-static.a / .lib）")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ★ 全库 PIC：GW_BUILD_DRIVER=ON 时 gw_driver 是共享库（.so / .dll），它要链接
#   静态库 gw_client / gw_common —— 静态库若不带 -fPIC，Linux 上链接 .so 直接报
#   「relocation R_X86_64_TPOFF32 ... can not be used when making a shared object」。
#   Windows/MinGW 不报（PE 没有这类重定位限制），所以本机双编译器一直没暴露；
#   是 WSL 复验（wsl preset 带 GW_BUILD_DRIVER=ON）抓出来的。全局 ON 成本可忽略。
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Paho MQTT C（MQTT 传输 / W4-1 告警发布）─────────────────────────────────
# ★ 必须在 add_compile_options(-Wall ... -Werror) **之前** FetchContent：
#   第三方库不能继承我们的告警矩阵（GCC13 对 paho utf-8.c 有 -Woverflow，
#   继承 -Werror 会把 paho 自己的构建卡死）。GW_WITH_MQTT=OFF 或给了本地
#   paho 库路径时跳过（本地库由使用者自行按其规则构建）。
# ★ 为什么钉 v1.3.14 而不用 apt 的 1.3.13：1.3.13 的 Socket.c 在 connect
#   EINPROGRESS 后缺 Socket_addSocket —— 同进程第二个 MQTTAsync 客户端永远
#   等不到 CONNACK（W4-1 告警发布器实测挂死；v1.3.14 diff 补上 addSocket）。
if(GW_WITH_MQTT AND NOT GW_PAHO_INCLUDE_DIR)
  include(FetchContent)
  set(PAHO_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(PAHO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(PAHO_WITH_SSL OFF CACHE BOOL "" FORCE)
  set(PAHO_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  FetchContent_Declare(paho-mqtt-c
    GIT_REPOSITORY https://github.com/eclipse/paho.mqtt.c.git
    GIT_TAG        v1.3.14
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(paho-mqtt-c)
  # 消费者（gw_mqtt / gw_alerts / mqtt_e2e）把 paho 头当 SYSTEM ——
  # 第三方头的 -Wpedantic 告警（匿名 struct 等）不该卡我们的 -Werror
  set_property(TARGET paho-mqtt3a-static APPEND PROPERTY
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${paho-mqtt-c_SOURCE_DIR}/src")
endif()

# ★ enable_testing() 必须在任何 add_test 之前调用。
#   曾经把它放在 add_subdirectory(apps/...) 之后，导致 apps/ 里注册的
#   gw_sim_selftest / proxy_demo_scenario 被静默丢弃 —— CI 的 ctest 会少跑三个测试，
#   而且不报任何错。用 `ctest -N` 核对测试清单能立刻发现这类问题。
enable_testing()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
  if(GW_WERROR)
    add_compile_options(-Werror)
  endif()
endif()

if(GW_FETCH_DEPS)
  include(FetchContent)
  # GoogleTest：只在系统里找不到时才拉取，避免污染 WSL 的 apt 安装
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        release-1.12.1)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()

add_subdirectory(common)

if(GW_BUILD_DEMO)
  add_subdirectory(apps/proto_demo)
endif()

if(GW_BUILD_PROXY)
  add_subdirectory(apps/proxy_demo)
endif()

if(GW_BUILD_SIM)
  add_subdirectory(apps/sim)
endif()

# gw_client：模拟器与驱动插件**共用**的协议客户端 —— 任一目标开启都要建。
# ★ P0-3 真头联调实测的坑：它曾只挂在 GW_BUILD_SIM 之下，于是「只开
#   GW_BUILD_DRIVER（真 Neuron 的构建场景）」时 gw_client 目标不存在，
#   CMake 把链接名静默降级成 -lgw_client —— 传递 include 全部丢失，
#   表现为 driver_impl.cpp 找不到 gw/client.h。别把它重新塞回 SIM 的 gate 里。
if(GW_BUILD_SIM OR GW_BUILD_DRIVER)
  add_subdirectory(client)
endif()

# SQLite 持久化记录库（W3 环形缓存的落盘实现）。
# 依赖系统 sqlite3 开发包（Ubuntu: libsqlite3-dev）；Windows 本机默认关
# （需要时用 amalgamation 编一个即可）。IRecordStore 契约在 gw_common，零依赖。
option(GW_BUILD_SQLITE "构建 SQLite 持久化记录库（W3 环形缓存）" OFF)
if(GW_BUILD_SQLITE)
  add_subdirectory(store)
endif()

# 规则引擎（W3-3）：阈值/变化率 → 告警。FetchContent 拉取 nlohmann/json（需网络）。
option(GW_BUILD_RULES "构建规则引擎（W3-3，FetchContent nlohmann/json）" OFF)
if(GW_BUILD_RULES)
  add_subdirectory(rules)
endif()

option(GW_BUILD_DRIVER "构建 Neuron 驱动插件（自研件②）" OFF)

if(GW_BUILD_DRIVER)
  add_subdirectory(plugins/driver)
endif()

if(GW_WITH_MQTT)
  add_subdirectory(mqtt)
  add_subdirectory(apps/mqtt_e2e)
endif()

if(GW_BUILD_TESTS)
  add_subdirectory(tests)
endif()

message(STATUS "──────────────────────────────────────────────")
message(STATUS " industrial_gateway  ${PROJECT_VERSION}")
message(STATUS "   build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "   compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "   werror     : ${GW_WERROR}")
message(STATUS "──────────────────────────────────────────────")
