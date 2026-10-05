set(GW_TEST_SOURCES
    test_main.cpp
    test_crc16.cpp
    test_frame.cpp
    test_parser.cpp
    test_register_table.cpp
    test_injection.cpp
    test_device_model.cpp
    test_reliability.cpp
)

add_executable(gw_tests ${GW_TEST_SOURCES})
target_link_libraries(gw_tests PRIVATE gw_common)
target_include_directories(gw_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
