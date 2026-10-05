add_executable(mqtt_e2e main.cpp)
target_link_libraries(mqtt_e2e PRIVATE gw_mqtt)

# 需要 asio（最小 broker + 自带线程）
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
# SYSTEM：asio 自己的头在 -Wshadow 下有告警（见 apps/sim/CMakeLists.txt 的说明）
target_include_directories(mqtt_e2e SYSTEM PRIVATE "${GW_ASIO_HEADERS}")
target_compile_definitions(mqtt_e2e PRIVATE ASIO_STANDALONE)
if(WIN32)
    target_compile_definitions(mqtt_e2e PRIVATE _WIN32_WINNT=0x0601)
    # mswsock 必需：asio 的 IOCP accept 路径引用 GetAcceptExSockaddrs
    target_link_libraries(mqtt_e2e PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mqtt_e2e PRIVATE Threads::Threads)
endif()

# 端到端自检：进程内起最小 broker，脚本化制造断网，自动对账
add_test(NAME mqtt_e2e_selftest COMMAND mqtt_e2e --selftest)
# 给足超时：自检含一次性断网重连
set_tests_properties(mqtt_e2e_selftest PROPERTIES TIMEOUT 180)

# 吞吐对比（规模调小以便进 CI；本地要看可信曲线时手动加大 --count）
add_test(NAME mqtt_e2e_bench COMMAND mqtt_e2e --bench --count 60 --points 30)
set_tests_properties(mqtt_e2e_bench PROPERTIES TIMEOUT 180)
