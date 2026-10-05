# NGWP 同步客户端（设备侧对话方）。插件与自检都用它。
# 需要 asio（纯头文件）—— 先把 asio 头文件目录解析出来（与 apps/sim 同样的两种来源）。
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

add_library(gw_client STATIC src/client.cpp)
target_link_libraries(gw_client PUBLIC gw_common)
target_include_directories(gw_client PUBLIC include)
# SYSTEM：asio 自己的头在 -Wshadow 下有告警，而本项目全局 -Werror
target_include_directories(gw_client SYSTEM PRIVATE "${GW_ASIO_HEADERS}")
target_compile_definitions(gw_client PRIVATE ASIO_STANDALONE)
if(WIN32)
    target_compile_definitions(gw_client PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(gw_client PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(gw_client PRIVATE Threads::Threads)
endif()
