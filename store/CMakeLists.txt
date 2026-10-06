# SQLite 持久化记录库（W3 环形缓存的落盘实现）
#
# 与 gw_mqtt 同款隔离策略：sqlite3.h 被 PIMPL 挡在 .cpp 里，
# gw_common（IRecordStore 契约所在）保持零外部依赖。
# 直接用 sqlite3 C API 而非 SQLiteCpp：SQL 面积极小，少一个第三方依赖。
find_package(SQLite3 REQUIRED)

add_library(gw_store_sqlite STATIC src/sqlite_store.cpp)
target_link_libraries(gw_store_sqlite PUBLIC gw_common SQLite::SQLite3)
target_include_directories(gw_store_sqlite PUBLIC include)
