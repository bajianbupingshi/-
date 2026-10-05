# asio 是纯头文件库、仓库里没有 CMakeLists.txt，
# 所以只能用 FetchContent 拿到源码目录后手动指定 include 路径。
# 离线/本机验证时可用 -DGW_ASIO_INCLUDE_DIR=<路径> 指向已有的 asio/include。
if(GW_ASIO_INCLUDE_DIR)
    set(GW_ASIO_HEADERS "${GW_ASIO_INCLUDE_DIR}")
    message(STATUS "sim: 使用本地 asio 头文件 -> ${GW_ASIO_HEADERS}")
else()
    include(FetchContent)
    FetchContent_Declare(asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    TRUE)
    # asio 没有 CMakeLists.txt，MakeAvailable 会跳过 add_subdirectory 只做 populate
    FetchContent_MakeAvailable(asio)
    set(GW_ASIO_HEADERS "${asio_SOURCE_DIR}/asio/include")
    message(STATUS "sim: FetchContent 取到 asio -> ${GW_ASIO_HEADERS}")
endif()

if(NOT EXISTS "${GW_ASIO_HEADERS}/asio.hpp")
    message(FATAL_ERROR "asio 头文件找不到：${GW_ASIO_HEADERS}/asio.hpp")
endif()

add_executable(gw_sim main.cpp)
target_link_libraries(gw_sim PRIVATE gw_common gw_client)
# ★ 必须用 SYSTEM：asio 自己的头文件在 -Wshadow 下有告警
#   （examples: detail/impl/win_static_mutex.ipp 里 `HANDLE mutex` 遮蔽全局 mutex），
#   而本项目全局开了 -Werror。不加 SYSTEM 会被第三方代码卡死编译。
target_include_directories(gw_sim SYSTEM PRIVATE "${GW_ASIO_HEADERS}")
target_compile_definitions(gw_sim PRIVATE ASIO_STANDALONE)

if(WIN32)
    target_compile_definitions(gw_sim PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(gw_sim PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(gw_sim PRIVATE Threads::Threads)
endif()

# 端到端自检登记进 CTest：不需要任何外部客户端（自己起服务器 + 自己连）
add_test(NAME gw_sim_selftest COMMAND gw_sim --selftest)

# 客户端自检：NgwpClient 对着进程内服务器（插件要用的协议客户端这一层）
add_test(NAME gw_client_selftest COMMAND gw_sim --client-selftest)
