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
  add_subdirectory(client)
  add_subdirectory(apps/sim)
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
