#ifndef GW_SQLITE_STORE_H
#define GW_SQLITE_STORE_H

#include <cstdint>
#include <memory>
#include <string>

#include "gw/reliability.h"

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// SQLite 持久化版记录队列（W3 环形缓存的落盘实现）
//
// 与内存版 RecordStore 实现**同一份 IRecordStore 契约**（同一组单测钉住两边）：
//   · 容量满丢「最新」并计 dropped（与内存版一致：队列头部 seq 保持连续）
//   · ack(seq) 删除 seq 及之前的行（已确认即回收）
//   · expire(now) 按 TTL 删除过期行并计 expired
//
// 持久化语义（这是它存在的意义）：
//   · 记录落 SQLite（WAL 模式）—— 进程被杀（SIGKILL）后重启，未确认记录还在
//   · seq 水位线：BackfillController 每分配一个 seq 经 note_seq() 落一张 meta 表，
//     重启后 last_seq_hint() 返回它 ⇒ seq 绝不重用（对账与去重的根基）
//   · 计数器（dropped/expired/acked）是进程本地统计，重启归零 —— 持久化的是数据不是统计
//
// 已知边界（诚实声明）：
//   · 非线程安全（与内存版一致）；多线程化是 W3 后续工作，届时需在层外串行化
//   · WAL + synchronous=NORMAL：进程崩溃零丢失；**断电**可能丢最后一笔事务
//     （环形缓存场景可接受，TTL 过期本来就是丢）—— 要更强就改 synchronous=FULL
//   · 直接用 sqlite3 C API 而非 SQLiteCpp：本场景 SQL 面积极小（5 条语句），
//     少一个第三方依赖；sqlite3.h 被 PIMPL 挡在 .cpp 里，头文件零依赖
//   · 「已分配未落库」窗口（note_seq 之后、push 之前崩溃）会丢该条 seq ——
//     与内存版「崩溃全丢」相比已是数量级的改善
// ─────────────────────────────────────────────────────────────────────────────
class SqliteRecordStore : public IRecordStore {
public:
    // db_path 打开/创建（目录必须已存在）；capacity 满时拒新留旧（同内存版）；
    // ttl_ms 为过期窗口。重复打开同一个库是合法的（重启续传的用法）。
    SqliteRecordStore(const std::string& db_path, std::size_t capacity, std::uint64_t ttl_ms);
    ~SqliteRecordStore() override;
    SqliteRecordStore(const SqliteRecordStore&) = delete;
    SqliteRecordStore& operator=(const SqliteRecordStore&) = delete;

    bool push(const Record& rec) override;
    void ack(std::uint64_t seq) override;
    void expire(std::uint64_t now_ms) override;

    std::size_t size() const override;
    bool empty() const override;
    const Record& front() const override;

    std::uint64_t dropped() const override;
    std::uint64_t expired() const override;
    std::uint64_t highest_seq() const override;
    std::uint64_t lowest_seq() const override;
    std::uint64_t acked_count() const override;

    void note_seq(std::uint64_t seq) override;
    std::uint64_t last_seq_hint() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gw

#endif  // GW_SQLITE_STORE_H
