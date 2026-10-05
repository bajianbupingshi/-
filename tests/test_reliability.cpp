// 边缘数据代理的可靠性内核测试
//
// 这一组测试存在的意义：让「断网 10 分钟续传丢失 0、重复 0」成为一条**可复现的自动化指标**，
// 而不是一句写在简历上的话。因此包含三类：
//   ① 语义单测：队列/水位/对账器各自的边界
//   ② 场景测试：用注入时钟把「断网 10 分钟」瞬间跑完，数出真实条数
//   ③ ★ 反转断言：故意制造丢包，对账器**必须**报出丢失 ——
//      只有它会报错，「丢失 0」才不是废话。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gtest_shim.h"
#include "gw/reliability.h"

namespace {

// 可推进的假时钟：整条时序瞬间跑完且完全确定
class FakeClock : public gw::IClock {
public:
    std::uint64_t now_ms() const override { return now_; }
    void advance(std::uint64_t ms) { now_ += ms; }
    void set(std::uint64_t ms) { now_ = ms; }

private:
    std::uint64_t now_ = 0;
};

// 可编程的假传输：
//   down         —— 链路断开，send 返回 Retry
//   drop_every_n —— 每 n 次「成功」里制造一次静默丢失（对端没收到，但返回 Ok）
// 后者用来做反转断言：对账器必须能发现这种丢包。
class FakeTransport : public gw::ITransport {
public:
    FakeTransport(gw::Ledger& ledger, FakeClock& clock)
        : ledger_(ledger), clock_(clock) {}

    gw::SendResult send(const gw::Record& rec) override {
        ++attempts;
        if (down) {
            return gw::SendResult::Retry;
        }
        if (drop_every_n > 0 && (++sent_ok % drop_every_n) == 0) {
            dropped_by_network.push_back(rec.seq);
            return gw::SendResult::Ok;  // 假装成功 —— 这正是最危险的情形
        }
        gw::Record r = rec;
        r.ts_ms = clock_.now_ms();
        ledger_.accept(r);
        order.push_back(rec.seq);
        return gw::SendResult::Ok;
    }

    bool down = false;
    std::uint64_t drop_every_n = 0;
    std::uint64_t attempts = 0;
    std::uint64_t sent_ok = 0;
    std::vector<std::uint64_t> order;             // 实际送达顺序
    std::vector<std::uint64_t> dropped_by_network;

private:
    gw::Ledger& ledger_;
    FakeClock& clock_;
};

// 采样周期 250ms，跑 total_ms 时长
void run_samples(gw::DataProxy& proxy, FakeClock& clock,
                 std::uint64_t total_ms, std::uint64_t period_ms) {
    for (std::uint64_t t = 0; t < total_ms; t += period_ms) {
        proxy.sample(0, static_cast<std::uint16_t>(t / period_ms));
        clock.advance(period_ms);
        proxy.tick();
    }
}

// 便捷构造：一条**单点位**消息（投递单元是"消息"，里面可以有 N 个点位）
gw::Record rec1(std::uint64_t seq, std::uint64_t ts, std::uint16_t addr = 0,
                std::uint16_t value = 0) {
    gw::Record r;
    r.seq = seq;
    r.ts_ms = ts;
    r.points.push_back(gw::Point{addr, value});
    return r;
}

// 便捷构造：一条 **N 点位**的消息
gw::Record recN(std::uint64_t seq, std::uint64_t ts, std::size_t n) {
    gw::Record r;
    r.seq = seq;
    r.ts_ms = ts;
    for (std::size_t i = 0; i < n; ++i) {
        r.points.push_back(gw::Point{static_cast<std::uint16_t>(i),
                                     static_cast<std::uint16_t>(seq * 100 + i)});
    }
    return r;
}

bool is_sorted_ascending(const std::vector<std::uint64_t>& v) {
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] <= v[i - 1]) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ── ① 语义单测 ──────────────────────────────────────────────────────────────
TEST(RecordStore, 容量打满时拒收最新一条并计数) {
    gw::RecordStore store(4, 3600000);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        EXPECT_TRUE(store.push(rec1(i, i * 10, 0, static_cast<std::uint16_t>(i))));
    }
    EXPECT_EQ(store.size(), std::size_t{4});
    EXPECT_FALSE(store.push(rec1(5, 50, 0, 5)));
    EXPECT_EQ(store.size(), std::size_t{4});
    EXPECT_EQ(store.dropped(), std::uint64_t{1});
    EXPECT_EQ(store.lowest_seq(), std::uint64_t{1});  // 队头 seq 仍连续
}

TEST(RecordStore, ack推进水位并回收) {
    gw::RecordStore store(16, 3600000);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        store.push(rec1(i, i, 0, 0));
    }
    store.ack(3);
    EXPECT_EQ(store.size(), std::size_t{2});
    EXPECT_EQ(store.lowest_seq(), std::uint64_t{4});
    EXPECT_EQ(store.acked_count(), std::uint64_t{3});
    store.ack(100);  // 超过已有范围也应安全
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.acked_count(), std::uint64_t{5});
}

TEST(RecordStore, TTL过期计为expired是真实丢失) {
    gw::RecordStore store(16, 1000);
    store.push(rec1(1, 0, 0, 0));
    store.push(rec1(2, 900, 0, 0));
    store.expire(900);                       // 两条都还在 TTL 内（0 和 900 的年龄分别为 900、0）
    EXPECT_EQ(store.size(), std::size_t{2});
    EXPECT_EQ(store.expired(), std::uint64_t{0});
    store.expire(1600);                      // 第 1 条(ts=0) 年龄 1600 > 1000 → 过期
    EXPECT_EQ(store.size(), std::size_t{1}); // 第 2 条(ts=900) 年龄 700 ≤ 1000 → 留下
    EXPECT_EQ(store.expired(), std::uint64_t{1});
}

TEST(Ledger, 去重与缺口统计) {
    gw::Ledger ledger;
    ledger.accept(rec1(1, 0, 0, 0));
    ledger.accept(rec1(2, 0, 0, 0));
    ledger.accept(rec1(2, 0, 0, 0));   // 重复投递
    ledger.accept(rec1(4, 0, 0, 0));   // 缺 3
    EXPECT_EQ(ledger.total_received(), std::uint64_t{4});
    EXPECT_EQ(ledger.unique_count(), std::uint64_t{3});
    EXPECT_EQ(ledger.duplicate_count(), std::uint64_t{1});
    EXPECT_EQ(ledger.missing_within_received(), std::uint64_t{1});
    const std::vector<std::uint64_t> gaps = ledger.missing_sample(8);
    ASSERT_EQ(gaps.size(), std::size_t{1});
    EXPECT_EQ(gaps[0], std::uint64_t{3});
}

// ── 批量打包：投递单元是「一条消息」，里面可以有 N 个点位 ────────────────────
TEST(Batch, 重复投递的批次不重复累加点位) {
    gw::Ledger ledger;
    ledger.accept(recN(1, 100, 5));
    ledger.accept(recN(2, 200, 5));
    ledger.accept(recN(2, 200, 5));   // 同一条消息被投递两次
    EXPECT_EQ(ledger.unique_count(), std::uint64_t{2});
    EXPECT_EQ(ledger.duplicate_count(), std::uint64_t{1});
    EXPECT_EQ(ledger.total_received(), std::uint64_t{3});
    EXPECT_EQ(ledger.points_received(), std::uint64_t{10});   // 5+5，重复那次不计
    EXPECT_EQ(ledger.missing_against_produced(2), std::uint64_t{0});
}

TEST(Batch, 一个周期多点位只产生一条消息) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(1000, 3600000);
    gw::DataProxy proxy(store, tx, clock);

    for (std::uint16_t i = 0; i < 50; ++i) {
        proxy.add_point(i, static_cast<std::uint16_t>(1000 + i));
    }
    EXPECT_EQ(proxy.pending_points(), std::size_t{50});
    const std::size_t emitted = proxy.end_cycle();
    EXPECT_EQ(emitted, std::size_t{50});
    EXPECT_EQ(proxy.pending_points(), std::size_t{0});

    EXPECT_EQ(proxy.stats().produced, std::uint64_t{1});   // ★ 只有 1 条消息
    EXPECT_EQ(proxy.stats().points, std::uint64_t{50});    // ★ 50 个点位
    EXPECT_EQ(ledger.unique_count(), std::uint64_t{1});
    EXPECT_EQ(ledger.points_received(), std::uint64_t{50});
    EXPECT_EQ(ledger.missing_against_produced(proxy.produced_highest_seq()), std::uint64_t{0});
}

TEST(Batch, 空周期不产生消息) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(1000, 3600000);
    gw::DataProxy proxy(store, tx, clock);
    EXPECT_EQ(proxy.end_cycle(), std::size_t{0});
    EXPECT_EQ(proxy.stats().produced, std::uint64_t{0});
}

TEST(Batch, 点位超过单条上限时自动拆成多条) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(1000, 3600000);
    gw::ProxyConfig cfg;
    cfg.max_points_per_message = 2;      // 故意设小以便测试
    gw::DataProxy proxy(store, tx, clock, cfg);

    for (std::uint16_t i = 0; i < 5; ++i) {
        proxy.add_point(i, i);
    }
    EXPECT_EQ(proxy.end_cycle(), std::size_t{5});
    EXPECT_EQ(proxy.stats().produced, std::uint64_t{3});   // ceil(5/2)
    EXPECT_EQ(proxy.stats().points, std::uint64_t{5});
    EXPECT_EQ(ledger.unique_count(), std::uint64_t{3});
    EXPECT_EQ(ledger.points_received(), std::uint64_t{5});
}

TEST(Batch, 单点模式与批量模式等价于一条消息一个点位) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(1000, 3600000);
    gw::DataProxy proxy(store, tx, clock);
    proxy.sample(7, 42);                 // 兼容旧口径
    EXPECT_EQ(proxy.stats().produced, std::uint64_t{1});
    EXPECT_EQ(proxy.stats().points, std::uint64_t{1});
    EXPECT_EQ(ledger.points_received(), std::uint64_t{1});
}

TEST(Batch, 断网期间积压的是消息而点位数另行统计) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(10000, 3600000);
    gw::DataProxy proxy(store, tx, clock);

    tx.down = true;
    for (int cycle = 0; cycle < 10; ++cycle) {          // 10 个周期 × 20 点位
        for (std::uint16_t i = 0; i < 20; ++i) {
            proxy.add_point(i, i);
        }
        proxy.end_cycle();
        clock.advance(100);
        proxy.tick();
    }
    EXPECT_EQ(proxy.stats().produced, std::uint64_t{10});   // 10 条消息积压
    EXPECT_EQ(proxy.stats().points, std::uint64_t{200});    // 200 个点位
    EXPECT_EQ(proxy.stats().backlog_peak, std::uint64_t{10});
    EXPECT_EQ(store.size(), std::size_t{10});

    tx.down = false;
    for (int i = 0; i < 200; ++i) {
        clock.advance(100);
        proxy.tick();
    }
    EXPECT_EQ(store.size(), std::size_t{0});
    EXPECT_EQ(ledger.unique_count(), std::uint64_t{10});
    EXPECT_EQ(ledger.points_received(), std::uint64_t{200});
    EXPECT_EQ(ledger.missing_against_produced(proxy.produced_highest_seq()), std::uint64_t{0});
    EXPECT_EQ(ledger.points_received(), proxy.stats().points);
}

// ★ 两个对账口径的差别必须被显式钉住 —— 我自己就在这上面判断错过一次。
TEST(Ledger, 两个对账口径的差别) {
    gw::Ledger ledger;
    for (std::uint64_t s = 1; s <= 100; ++s) {   // 接收端只看到 1..100，内部完全连续
        ledger.accept(rec1(s, 0, 0, 0));
    }
    // 口径①：接收侧自洽 —— 内部没有空洞
    EXPECT_EQ(ledger.missing_within_received(), std::uint64_t{0});
    // 口径②：端到端 —— 生产端其实产了 120 条，尾部 20 条从未发出
    EXPECT_EQ(ledger.missing_against_produced(120), std::uint64_t{20});
    // 只看口径① 会得出「丢失 0」的假结论
}

// ── ② 状态机转换 ────────────────────────────────────────────────────────────
TEST(Backfill, 断开会进入Backfill恢复后经CatchingUp回到Live) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(1000, 3600000);
    gw::DataProxy proxy(store, tx, clock);

    proxy.sample(0, 1);
    EXPECT_EQ(proxy.state(), gw::LinkState::Live);

    tx.down = true;
    proxy.sample(0, 2);
    EXPECT_EQ(proxy.state(), gw::LinkState::Backfill);

    tx.down = false;
    clock.advance(1000);        // 越过退避窗口
    proxy.tick();
    // 积压排空后应回到 Live
    EXPECT_EQ(proxy.state(), gw::LinkState::Live);
    EXPECT_EQ(proxy.stats().reconnects, std::uint64_t{1});
    EXPECT_EQ(store.size(), std::size_t{0});
}

TEST(Backfill, 指数退避按倍数增长且不超过上限) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(1000, 3600000);
    gw::BackfillConfig cfg;
    cfg.backoff_initial_ms = 100;
    cfg.backoff_max_ms = 800;
    cfg.backoff_multiplier = 2;
    gw::BackfillController ctrl(store, tx, clock, cfg);

    tx.down = true;
    ctrl.on_sample(rec1(1, 0, 0, 0));
    EXPECT_EQ(ctrl.state(), gw::LinkState::Backfill);

    // 连续在退避窗口外重试都失败 ⇒ 间隔翻倍：100,200,400,800,800…
    const std::uint32_t expect[] = {100, 200, 400, 800, 800};
    for (std::uint32_t want : expect) {
        clock.advance(want);
        ctrl.on_tick();
    }
    EXPECT_EQ(ctrl.stats().max_backoff_ms, std::uint64_t{800});
    EXPECT_GE(ctrl.stats().backoff_waits, std::uint64_t{5});
}

TEST(Backfill, 退避窗口内不重试) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(1000, 3600000);
    gw::BackfillConfig cfg;
    cfg.backoff_initial_ms = 500;
    gw::BackfillController ctrl(store, tx, clock, cfg);

    tx.down = true;
    ctrl.on_sample(rec1(1, 0, 0, 0));
    const std::uint64_t attempts_after_first = tx.attempts;

    clock.advance(100);   // 窗口内
    ctrl.on_tick();
    ctrl.on_tick();
    EXPECT_EQ(tx.attempts, attempts_after_first);  // 一次都不该试

    clock.advance(500);   // 越过窗口
    ctrl.on_tick();
    EXPECT_GT(tx.attempts, attempts_after_first);
}

// ── ③ ★ 主场景：断网 10 分钟，续传丢失 0、重复 0 ────────────────────────────
TEST(Reliability, 断网10分钟后续传丢失0重复0) {
    constexpr std::uint64_t kPeriodMs = 250;       // 采样周期
    constexpr std::uint64_t kOutageMs = 10 * 60 * 1000;  // 断网 10 分钟
    constexpr std::uint64_t kTotalMs = 30 * 60 * 1000;   // 总共跑 30 分钟

    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::ProxyConfig cfg;
    cfg.store_capacity = 20000;
    cfg.store_ttl_ms = 3600000;                    // TTL 1 小时 > 断网 10 分钟
    gw::RecordStore store(cfg.store_capacity, cfg.store_ttl_ms);
    gw::DataProxy proxy(store, tx, clock, cfg);

    bool outage_done = false;
    for (std::uint64_t t = 0; t < kTotalMs; t += kPeriodMs) {
        // t = 5 分钟时断开，持续 10 分钟
        if (t == 5 * 60 * 1000) {
            tx.down = true;
        }
        if (!outage_done && t == 5 * 60 * 1000 + kOutageMs) {
            tx.down = false;
            outage_done = true;
        }
        proxy.sample(0, static_cast<std::uint16_t>(t / kPeriodMs));
        clock.advance(kPeriodMs);
        proxy.tick();
    }

    const std::uint64_t produced = proxy.stats().produced;
    const std::uint64_t unique = ledger.unique_count();
    const std::uint64_t dup = ledger.duplicate_count();
    // ★ 端到端对账：拿生产端最高 seq 去比，而不是接收端自己的最大 seq
    const std::uint64_t missing = ledger.missing_against_produced(proxy.produced_highest_seq());

    std::printf("      [数据] 产出 %llu 条 / 接收去重 %llu 条 / 丢失 %llu / 重复 %llu\n",
                static_cast<unsigned long long>(produced),
                static_cast<unsigned long long>(unique),
                static_cast<unsigned long long>(missing),
                static_cast<unsigned long long>(dup));
    std::printf("      [数据] 积压峰值 %llu / 重连 %llu 次 / 退避上限 %llu ms / 容量丢弃 %llu / TTL 过期 %llu\n",
                static_cast<unsigned long long>(proxy.stats().backlog_peak),
                static_cast<unsigned long long>(proxy.stats().reconnects),
                static_cast<unsigned long long>(proxy.stats().max_backoff_ms),
                static_cast<unsigned long long>(store.dropped()),
                static_cast<unsigned long long>(store.expired()));

    EXPECT_GT(produced, std::uint64_t{6000});       // 30 分钟 / 250ms = 7200 条
    EXPECT_EQ(missing, std::uint64_t{0});           // ★ 丢失 0（端到端口径）
    EXPECT_EQ(dup, std::uint64_t{0});               // ★ 重复 0
    EXPECT_EQ(ledger.missing_within_received(), std::uint64_t{0});  // 口径① 也应自洽
    EXPECT_EQ(unique, produced);                    // 一条不少
    EXPECT_EQ(store.dropped(), std::uint64_t{0});   // 容量没打满
    EXPECT_EQ(store.expired(), std::uint64_t{0});   // TTL 没误杀
    EXPECT_EQ(store.size(), std::size_t{0});        // 全部排空
    EXPECT_EQ(proxy.state(), gw::LinkState::Live);
    EXPECT_EQ(proxy.stats().reconnects, std::uint64_t{1});
    // 断网 10 分钟的数据必须全在积压里：10min / 250ms = 2400 条左右
    EXPECT_GT(proxy.stats().backlog_peak, std::uint64_t{2000});
}

TEST(Reliability, 续传期间严格按序列号升序发送) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(20000, 3600000);
    gw::DataProxy proxy(store, tx, clock);

    tx.down = true;
    for (int i = 0; i < 500; ++i) {
        proxy.sample(0, static_cast<std::uint16_t>(i));
        clock.advance(10);
        proxy.tick();
    }
    tx.down = false;
    for (int i = 0; i < 200; ++i) {   // 给足 tick 让积压排空
        clock.advance(100);
        proxy.tick();
    }
    EXPECT_TRUE(is_sorted_ascending(tx.order));
    EXPECT_EQ(ledger.unique_count(), std::uint64_t{500});
    EXPECT_EQ(ledger.missing_against_produced(proxy.produced_highest_seq()), std::uint64_t{0});
}

// ── ④ ★ 反转断言：对账器必须能在真的丢包时报出丢失 ──────────────────────────
//
// 没有这一条，「丢失 0」就只是「检查器永远返回 0」的错觉。
TEST(Reliability, 反转断言_故意静默丢包时必须报出丢失) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    tx.drop_every_n = 97;                    // 每 97 次成功投递里静默丢 1 条
    gw::RecordStore store(20000, 3600000);
    gw::DataProxy proxy(store, tx, clock);

    run_samples(proxy, clock, 60000, 250);   // 1 分钟 ≈ 240 条

    EXPECT_GT(proxy.stats().produced, std::uint64_t{200});
    const std::uint64_t e2e = ledger.missing_against_produced(proxy.produced_highest_seq());
    EXPECT_GT(e2e, std::uint64_t{0});                             // ★ 必须报出丢失
    EXPECT_GT(tx.dropped_by_network.size(), std::size_t{0});
    EXPECT_EQ(e2e, static_cast<std::uint64_t>(tx.dropped_by_network.size()));  // 数量要对得上
    // 且缺的正是那些被静默丢掉的 seq
    const std::vector<std::uint64_t> gaps = ledger.missing_sample(16);
    EXPECT_EQ(gaps.front(), tx.dropped_by_network.front());
}

TEST(Reliability, 反转断言_TTL小于断网时长必然丢数据) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(20000, 60000);     // TTL 1 分钟 —— 明显小于下面的断网时长
    gw::DataProxy proxy(store, tx, clock);

    tx.down = true;
    run_samples(proxy, clock, 5 * 60 * 1000, 250);   // 断网 5 分钟
    tx.down = false;
    for (int i = 0; i < 300; ++i) {
        clock.advance(100);
        proxy.tick();
    }

    EXPECT_GT(store.expired(), std::uint64_t{0});        // 有过期丢弃
    const std::uint64_t e2e = ledger.missing_against_produced(proxy.produced_highest_seq());
    EXPECT_GT(e2e, std::uint64_t{0});                    // 因此必然丢数据
    std::printf("      [数据] TTL=60s 断网 5 分钟 → TTL 过期 %llu 条，端到端缺口 %llu 条\n",
                static_cast<unsigned long long>(store.expired()),
                static_cast<unsigned long long>(e2e));
}

TEST(Reliability, 反转断言_容量打满必然丢数据) {
    FakeClock clock;
    gw::Ledger ledger;
    FakeTransport tx(ledger, clock);
    gw::RecordStore store(100, 3600000);     // 只能存 100 条
    gw::DataProxy proxy(store, tx, clock);

    tx.down = true;
    run_samples(proxy, clock, 30000, 250);   // 断网期间产生 120 条
    tx.down = false;
    for (int i = 0; i < 300; ++i) {
        clock.advance(100);
        proxy.tick();
    }

    EXPECT_EQ(store.dropped(), std::uint64_t{20});       // 120 - 100
    // ★ 这里正是「两个口径」的实战差别：接收端见到的 1..100 内部是连续的
    //   （口径① 报 0），但生产端产了 120 条（口径② 报 20）。
    EXPECT_EQ(ledger.missing_within_received(), std::uint64_t{0});
    EXPECT_EQ(ledger.missing_against_produced(proxy.produced_highest_seq()), std::uint64_t{20});
}
