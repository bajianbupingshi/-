add_library(gw_common STATIC
    src/crc16.cpp
    src/frame.cpp
    src/parser.cpp
    src/register_table.cpp
    src/injection.cpp
    src/device_model.cpp
    src/reliability.cpp
    src/proxy_service.cpp
)

# W3-2：EdgeProxyService 的管道线程用 std::thread —— 使用者经 PUBLIC 传递拿到 pthread
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(gw_common PUBLIC Threads::Threads)
endif()

target_include_directories(gw_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(gw_common PUBLIC cxx_std_17)
