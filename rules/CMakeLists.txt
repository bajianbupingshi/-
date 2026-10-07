# 规则引擎（W3-3）：阈值 / 变化率 → 告警
#
# 与 gw_mqtt / gw_store_sqlite 同款隔离策略：
#   · nlohmann/json 只进 .cpp（公开头 gw/rules.h 零第三方依赖）
#   · 依赖 gw_common（Point / Alert 消费的数据类型）
# 选型说明：配置解析用方案 §4 点名的 nlohmann/json（FetchContent v3.11.3，
# GIT_SHALLOW 浅克隆）；引擎的求值部分纯 C++，不依赖任何 JSON。
include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(gw_rules STATIC src/rules.cpp)
target_link_libraries(gw_rules PUBLIC gw_common PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(gw_rules PUBLIC include)
