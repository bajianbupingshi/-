add_executable(proxy_demo main.cpp)
target_link_libraries(proxy_demo PRIVATE gw_common)

# 默认场景：30 分钟时长、第 5 分钟起断网 10 分钟 —— 应当丢失 0、重复 0
add_test(NAME proxy_demo_scenario COMMAND proxy_demo)
# 反转断言：注入静默丢包，对账器必须报出丢失
add_test(NAME proxy_demo_reversal COMMAND proxy_demo --drop-every 97)
