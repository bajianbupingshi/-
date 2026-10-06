// SQLite 持久化记录库的单测 —— 与内存版（test_reliability.cpp）钉同一份契约。
// 断言一律用 EXPECT_*/ASSERT_* 宏：内置垫片与真实 GoogleTest 两边都能跑。
//
// 三个存在理由：
//   ① 行为契约：push/ack/expire/容量丢弃的观测面与内存版逐一一致（可互换的前提）
//   ② 持久化：写满 → 关闭 → 重开，记录与水位线原样回来（断电续传的根基）
//   ③ 重启续传验收场景：proxy1 断网产出 → 「重启」→ proxy2 从水位线续传 →
//      跨重启的 Ledger 对账丢失 0 / 重复 0 —— 这是 W3-1 的验收标准
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gtest_shim.h"
#include "gw/sqlite_store.h"
#include "gw/reliability.h"

namespace {

const char* kDb = "test_sqlite_store.db";

void remove_db() {
    std::remove(kDb);
    std::remove((std::string(kDb) + "-wal").c_str());
    std::remove((std::string(kDb) + "-shm").c_str());
}

gw::Record make_record(std::uint64_t seq, std::uint64_t ts, int n_points) {
    gw::Record r;
    r.seq = seq;
    r.ts_ms = ts;
    for (int i = 0; i < n_points; ++i) {
        r.points.push_back(gw::Point{static_cast<std::uint16_t>(i),
                                     static_cast<std::uint16_t>(seq * 7 + i)});
    }
    return r;
}

// 与 proxy_demo 同款的注入时钟
class SimClock : public gw::IClock {
public:
    std::uint64_t now_ms() const override { return now_; }
    void advance(std::uint64_t ms) { now_ += ms; }

private:
    std::uint64_t now_ = 0;
};

// 可编程传输：down = 链路断开；up 后正常投递并对账（跨重启存活）
class SimTransport : public gw::ITransport {
public:
    explicit SimTransport(gw::Ledger& ledger) : ledger_(ledger) {}

    gw::SendResult send(const gw::Record& rec) override {
        if (down) {
            return gw::SendResult::Retry;
        }
        ledger_.accept(rec);
        return gw::SendResult::Ok;
    }

    bool down = false;

private:
    gw::Ledger& ledger_;
};

TEST(SqliteStore, RoundTripMatchesMemorySemantics) {
    remove_db();
    gw::SqliteRecordStore store(kDb, 100, 60000);

    EXPECT_TRUE(store.push(make_record(1, 1000, 1)));
    EXPECT_TRUE(store.push(make_record(2, 2000, 3)));
    EXPECT_EQ(store.size(), std::size_t{2});
    EXPECT_FALSE(store.empty());
    ASSERT_FALSE(store.empty());
    EXPECT_EQ(store.front().seq, std::uint64_t{1});
    ASSERT_EQ(store.front().points.size(), std::size_t{1});
    EXPECT_EQ(store.front().points[0].value, std::uint16_t{7});
    EXPECT_EQ(store.highest_seq(), std::uint64_t{2});
    EXPECT_EQ(store.lowest_seq(), std::uint64_t{1});

    store.ack(1);
    EXPECT_EQ(store.size(), std::size_t{1});
    EXPECT_EQ(store.acked_count(), std::uint64_t{1});
    ASSERT_FALSE(store.empty());
    EXPECT_EQ(store.front().seq, std::uint64_t{2});

    // TTL：now - ts > ttl 才过期（与内存版同款判定）
    store.expire(2000 + 60000);      // 恰好等于 TTL：不过期
    EXPECT_EQ(store.size(), std::size_t{1});
    store.expire(2000 + 60000 + 1);  // 超过 1ms：过期
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.expired(), std::uint64_t{1});
    EXPECT_EQ(store.dropped(), std::uint64_t{0});
}

TEST(SqliteStore, CapacityDropsNewestLikeMemory) {
    remove_db();
    gw::SqliteRecordStore store(kDb, 2, 600000);

    EXPECT_TRUE(store.push(make_record(1, 100, 1)));
    EXPECT_TRUE(store.push(make_record(2, 200, 1)));
    EXPECT_FALSE(store.push(make_record(3, 300, 1)));  // 容量满应拒收最新
    EXPECT_EQ(store.dropped(), std::uint64_t{1});
    EXPECT_EQ(store.size(), std::size_t{2});
    EXPECT_EQ(store.front().seq, std::uint64_t{1});    // 头部 seq 连续（丢最新的取舍）
}

TEST(SqliteStore, PersistenceAcrossReopen) {
    remove_db();
    {
        gw::SqliteRecordStore store(kDb, 100, 600000);
        for (std::uint64_t s = 1; s <= 3; ++s) {
            EXPECT_TRUE(store.push(make_record(s, s * 100, 2)));
            store.note_seq(s);
        }
        store.ack(1);   // 已确认的行落库时就删了，重启后不该回来
        EXPECT_EQ(store.size(), std::size_t{2});
    }   // 析构 = 正常关闭

    // 重开：未确认的 2 条原样回来，水位线不回退
    gw::SqliteRecordStore store(kDb, 100, 600000);
    EXPECT_EQ(store.size(), std::size_t{2});
    ASSERT_FALSE(store.empty());
    EXPECT_EQ(store.front().seq, std::uint64_t{2});
    ASSERT_EQ(store.front().points.size(), std::size_t{2});
    EXPECT_EQ(store.front().points[1].value, static_cast<std::uint16_t>(2 * 7 + 1));
    EXPECT_EQ(store.last_seq_hint(), std::uint64_t{3});
    EXPECT_EQ(store.highest_seq(), std::uint64_t{3});
}

TEST(SqliteStore, WatermarkSurvivesReopenAndPreventsReuse) {
    remove_db();
    {
        gw::SqliteRecordStore store(kDb, 100, 600000);
        // 模拟「分配到 100 但只落库到 5」：水位线由控制器经 note_seq 推进
        store.note_seq(100);
        for (std::uint64_t s = 1; s <= 5; ++s) {
            store.push(make_record(s, s, 1));
        }
        EXPECT_EQ(store.last_seq_hint(), std::uint64_t{100});
    }
    gw::SqliteRecordStore store(kDb, 100, 600000);
    EXPECT_EQ(store.last_seq_hint(), std::uint64_t{100});

    // 重启后的控制器从水位线之后继续分配 —— seq 101 起，绝不重用 1..100
    SimClock clock;
    gw::Ledger ledger;
    SimTransport tx(ledger);
    gw::BackfillController ctrl(store, tx, clock, {}, store.last_seq_hint());
    EXPECT_EQ(ctrl.next_seq(), std::uint64_t{101});
}

TEST(SqliteStore, RestartResumeScenario) {
    // ★ W3-1 验收场景：断网产出 → 「重启」（关库重开）→ 从水位线续传 →
    //   跨重启的 Ledger 对账丢失 0 / 重复 0
    remove_db();
    gw::Ledger ledger;   // 接收端对账器：跨重启存活（它就是「云端」）

    {
        // ── 第一段：链路断开，数据只落库 ──
        SimClock clock;
        gw::SqliteRecordStore store(kDb, 10000, 3600000);
        gw::Ledger sink;                       // 第一段没有任何东西到达对账器
        SimTransport tx(sink);
        tx.down = true;
        gw::DataProxy proxy(store, tx, clock, {}, 0);

        for (int i = 1; i <= 20; ++i) {
            proxy.sample(0, static_cast<std::uint16_t>(i));
            clock.advance(100);
            proxy.tick();
        }
        EXPECT_EQ(store.size(), std::size_t{20});
        EXPECT_EQ(ledger.unique_count(), std::uint64_t{0});
        EXPECT_EQ(proxy.produced_highest_seq(), std::uint64_t{20});
    }   // 进程「崩溃」：什么都不 flush，靠的是 SQLite 自己的持久化

    {
        // ── 第二段：重启。链路恢复，从水位线继续分配，排空积压 + 新数据 ──
        SimClock clock;
        clock.advance(60000);   // 重启发生在 1 分钟后（TTL 内）
        gw::SqliteRecordStore store(kDb, 10000, 3600000);
        ASSERT_EQ(store.size(), std::size_t{20});
        EXPECT_EQ(store.last_seq_hint(), std::uint64_t{20});

        SimTransport tx(ledger);   // 链路恢复
        gw::DataProxy proxy(store, tx, clock, {}, store.last_seq_hint());

        // 新数据照常采（seq 必须从 21 起 —— 水位线的意义）
        for (int i = 21; i <= 30; ++i) {
            proxy.sample(0, static_cast<std::uint16_t>(i));
            clock.advance(100);
            proxy.tick();
        }
        // 排空积压：老 20 条 + 新 10 条全部送达
        for (int i = 0; i < 1000 && !proxy.store().empty(); ++i) {
            clock.advance(100);
            proxy.tick();
        }
        EXPECT_TRUE(proxy.store().empty());
        EXPECT_EQ(proxy.state(), gw::LinkState::Live);
    }

    // ── 跨重启对账：30 条全部唯一、丢失 0、重复 0 ──
    EXPECT_EQ(ledger.unique_count(), std::uint64_t{30});
    EXPECT_EQ(ledger.missing_against_produced(30), std::uint64_t{0});
    EXPECT_EQ(ledger.duplicate_count(), std::uint64_t{0});
    for (std::uint64_t s = 1; s <= 30; ++s) {
        EXPECT_TRUE(ledger.has(s)) << "seq " << s << " 不应缺失";
    }
}

}  // namespace
