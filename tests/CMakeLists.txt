set(GW_TEST_SOURCES
    test_main.cpp
    test_crc16.cpp
    test_frame.cpp
    test_parser.cpp
    test_register_table.cpp
    test_injection.cpp
    test_device_model.cpp
    test_reliability.cpp
    test_bounded_queue.cpp
    test_proxy_service.cpp
)

# SQLite 持久化版（GW_BUILD_SQLITE=ON 时才存在）—— 与内存版钉同一份契约
if(TARGET gw_store_sqlite)
    list(APPEND GW_TEST_SOURCES test_sqlite_store.cpp)
endif()

# 规则引擎（GW_BUILD_RULES=ON 时才存在）
if(TARGET gw_rules)
    list(APPEND GW_TEST_SOURCES test_rules.cpp)
endif()

# 告警发布端到端（需要规则引擎 + MQTT + asio —— mini broker 是 asio 的）
# asio 头路径在这里解析；target 级接线挪到 add_executable 之后（目标须先存在）
set(GW_HAVE_ALERT_TEST OFF)
if(TARGET gw_alerts)
    list(APPEND GW_TEST_SOURCES test_alert_publish.cpp)
    set(GW_HAVE_ALERT_TEST ON)
    if(GW_ASIO_INCLUDE_DIR)
        set(GW_ASIO_HEADERS "${GW_ASIO_INCLUDE_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(asio
            GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
            GIT_TAG        asio-1-30-2
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(asio)
        set(GW_ASIO_HEADERS "${asio_SOURCE_DIR}/asio/include")
    endif()
endif()

add_executable(gw_tests ${GW_TEST_SOURCES})
target_link_libraries(gw_tests PRIVATE gw_common)
if(TARGET gw_store_sqlite)
    target_link_libraries(gw_tests PRIVATE gw_store_sqlite)
endif()
if(TARGET gw_rules)
    target_link_libraries(gw_tests PRIVATE gw_rules)
endif()
if(TARGET gw_alerts)
    target_link_libraries(gw_tests PRIVATE gw_alerts)
endif()
target_include_directories(gw_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(GW_HAVE_ALERT_TEST)
    # SYSTEM：asio 的头在 -Wshadow 下有告警（与 apps/sim 同款处理）
    target_include_directories(gw_tests SYSTEM PRIVATE "${GW_ASIO_HEADERS}")
    target_compile_definitions(gw_tests PRIVATE ASIO_STANDALONE)
    if(WIN32)
        target_compile_definitions(gw_tests PRIVATE _WIN32_WINNT=0x0601)
        target_link_libraries(gw_tests PRIVATE ws2_32 mswsock)
    endif()
endif()

# 优先用系统 GoogleTest；没有就用内置垫片（同一份测试源码，两边都能跑）
if(TARGET GTest::gtest_main)
    target_link_libraries(gw_tests PRIVATE GTest::gtest_main)
    target_compile_definitions(gw_tests PRIVATE GW_HAVE_GTEST=1)
    message(STATUS "tests: GoogleTest（含 gtest_main）")
elseif(TARGET GTest::gtest)
    target_link_libraries(gw_tests PRIVATE GTest::gtest)
    target_compile_definitions(gw_tests PRIVATE GW_HAVE_GTEST=1 GW_NEED_GTEST_MAIN=1)
    message(STATUS "tests: GoogleTest（无 gtest_main，由 test_main.cpp 提供 main）")
else()
    message(STATUS "tests: 未找到 GoogleTest -> 使用内置垫片 tests/gtest_shim.h")
endif()

add_test(NAME gw_tests COMMAND gw_tests)
