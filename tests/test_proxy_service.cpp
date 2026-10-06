// EdgeProxyService（W3-2 多线程服务壳）的单测 —— TSan 的主要检查对象。
//
// 线程模型：测试线程扮演「采集线程」调 produce()；服务的管道线程独占内核；
// 记录型传输把送达的数据喂进跨线程可见的 Ledger（互斥保护）——
// 全部共享点都在 TSan 的射程内。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "gtest_shim.h"
#include "gw/proxy_service.h"
#include "gw/reliability.h"

namespace {

// 记录型传输：送达的数据喂 Ledger（互斥保护 —— 对账器本身非线程安全）
class RecordingTransport : public gw::ITransport {
public:
    explicit RecordingTransport(gw::Ledger& ledger) : ledger_(ledger) {}

    gw::SendResult send(const gw::Record& rec) override {
        if (down.load()) {
            return gw::SendResult::Retry;
        }
        if (slow_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            ledger_.accept(rec);
            ++sent;
        }
        return gw::SendResult::Ok;
    }

    std::atomic<bool> down{false};
    std::atomic<int> slow_ms{0};
    mutable std::mutex mtx;   // 保护 ledger_：管道线程写，测试主线程读
    std::uint64_t sent = 0;

    // ★ 测试侧一律经这些快照读 Ledger —— 直接读会和管道线程的 accept() 竞争
    //   （TSan 首战实锤：_Rb_tree::size() 数据竞争）。真实系统里 Ledger 在
    //   「云端」另一个进程，测试里共享内存化后必须自己补上同步。
    std::uint64_t unique_count() const {
        std::lock_guard<std::mutex> lk(mtx);
        return ledger_.unique_count();
    }
    std::uint64_t duplicates() const {
        std::lock_guard<std::mutex> lk(mtx);
        return ledger_.duplicate_count();
    }
    std::uint64_t missing(std::uint64_t produced) const {
        std::lock_guard<std::mutex> lk(mtx);
        return ledger_.missing_against_produced(produced);
    }

private:
    gw::Ledger& ledger_;
};

std::vector<gw::Point> one_point(std::uint16_t addr, std::uint16_t value) {
    return {gw::Point{addr, value}};
}

// 轮询等待（真时钟）；超时返回 false 供断言
template <typename Pred>
bool wait_for(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

TEST(ProxyService, ProduceAndDrainNoLoss) {
    gw::Ledger ledger;
    RecordingTransport tx(ledger);
    gw::RecordStore store(100000, 3600000);
    gw::EdgeProxyService svc(store, tx, {}, 0, 1024, 5);
    svc.start();

    constexpr int kBatches = 500;
    for (int i = 1; i <= kBatches; ++i) {
        EXPECT_TRUE(svc.produce(one_point(0, static_cast<std::uint16_t>(i))));
    }
    // 全部送达（顺序 = 单生产者单消费者 + seq 保序）
    EXPECT_TRUE(wait_for([&] { return tx.unique_count() == std::uint64_t(kBatches); },
                         std::chrono::seconds(10)));
    EXPECT_EQ(svc.queued_dropped(), std::uint64_t{0});
    EXPECT_EQ(svc.state(), gw::LinkState::Live);

    svc.stop();
    EXPECT_EQ(tx.duplicates(), std::uint64_t{0});
    const gw::LinkStats st = svc.stats();
    EXPECT_EQ(st.produced, std::uint64_t{kBatches});
}

TEST(ProxyService, StopDrainsQueueGracefully) {
    // 优雅停止契约：stop() 前已 accept 的批次，排空后必须全部送达
    gw::Ledger ledger;
    RecordingTransport tx(ledger);
    gw::RecordStore store(100000, 3600000);
    gw::EdgeProxyService svc(store, tx, {}, 0, 4096, 5);
    svc.start();

    constexpr int kBatches = 50;
    for (int i = 1; i <= kBatches; ++i) {
        svc.produce(one_point(1, static_cast<std::uint16_t>(i)));
    }
    svc.stop();   // 立即停：管道应先把队列里的批次处理完再退出

    EXPECT_EQ(tx.unique_count(), std::uint64_t{kBatches});
    EXPECT_EQ(tx.missing(kBatches), std::uint64_t{0});
}

TEST(ProxyService, BackpressureDropsAreCounted) {
    // 背压契约：交接队列满 ⇒ produce 返回 false 且计入 queued_dropped ——
    // 采集侧的丢弃必须可见（与 store 容量丢弃、网络丢失是三个不同口径）
    gw::Ledger ledger;
    RecordingTransport tx(ledger);
    tx.slow_ms = 30;   // 拖慢管道，制造积压
    gw::RecordStore store(100000, 3600000);
    gw::EdgeProxyService svc(store, tx, {}, 0, /*queue_capacity=*/4, 5);
    svc.start();

    for (int i = 1; i <= 100; ++i) {
        svc.produce(one_point(2, static_cast<std::uint16_t>(i)));
    }
    // 管道每 30ms 才能消费一条，队列容量 4 ⇒ 100 条里必有大量入队丢弃
    EXPECT_GT(svc.queued_dropped(), std::uint64_t{0});

    svc.stop();
    // 已入队的 + 管道已取的 = 100 - 丢弃；对账数量与丢弃数精确互补
    const std::uint64_t received = tx.unique_count();
    EXPECT_EQ(received + svc.queued_dropped(), std::uint64_t{100});
}

TEST(ProxyService, OutageResumeRealTime) {
    // 真实时间线上的断网续传（注入时钟换 SteadyClock 后的语义验证）
    gw::Ledger ledger;
    RecordingTransport tx(ledger);
    tx.down = true;
    gw::RecordStore store(100000, 3600000);
    gw::EdgeProxyService svc(store, tx, {}, 0, 1024, 10);
    svc.start();

    constexpr int kOutageBatches = 20;
    for (int i = 1; i <= kOutageBatches; ++i) {
        svc.produce(one_point(3, static_cast<std::uint16_t>(i)));
    }
    // 断网期间：数据落库不发送
    EXPECT_TRUE(wait_for([&] { return svc.stats().produced >= kOutageBatches; },
                         std::chrono::seconds(10)));
    EXPECT_EQ(tx.unique_count(), std::uint64_t{0});

    // 链路恢复：积压排空
    tx.down = false;
    EXPECT_TRUE(wait_for([&] { return tx.unique_count() == std::uint64_t(kOutageBatches); },
                         std::chrono::seconds(30)));
    EXPECT_EQ(tx.missing(kOutageBatches), std::uint64_t{0});
    EXPECT_EQ(svc.state(), gw::LinkState::Live);
    svc.stop();
}

}  // namespace
