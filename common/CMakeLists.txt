add_library(gw_common STATIC
    src/crc16.cpp
    src/frame.cpp
    src/parser.cpp
    src/register_table.cpp
    src/injection.cpp
    src/device_model.cpp
    src/reliability.cpp
)

target_include_directories(gw_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(gw_common PUBLIC cxx_std_17)
