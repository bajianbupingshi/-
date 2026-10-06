// 有界阻塞队列的多线程测试 —— W3-2 的边界同步件，TSan 的直接检查对象。
// 契约：SPSC 使用下保序、满拒收、close 唤醒且排空后报 Closed、阻塞版可被 close 解救。
#include <atomic>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

#include "gtest_shim.h"
#include "gw/bounded_queue.h"

namespace {

TEST(BoundedQueue, BasicAndFull) {
    gw::BoundedQueue<int> q(2);
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_FALSE(q.push(3));           // 满
    EXPECT_EQ(q.size(), std::size_t{2});

    int v = 0;
    EXPECT_EQ(q.try_pop(v), gw::BoundedQueue<int>::PopResult::Ok);
    EXPECT_EQ(v, 1);
    EXPECT_EQ(q.try_pop(v), gw::BoundedQueue<int>::PopResult::Ok);
    EXPECT_EQ(v, 2);
    EXPECT_EQ(q.try_pop(v), gw::BoundedQueue<int>::PopResult::Empty);
}

TEST(BoundedQueue, CloseWakesBlockedConsumer) {
    gw::BoundedQueue<int> q(4);
    std::atomic<bool> consumer_done{false};
    gw::BoundedQueue<int>::PopResult got = gw::BoundedQueue<int>::PopResult::Empty;

    std::thread consumer([&] {
        int v = 0;
        got = q.wait_pop(v, std::chrono::seconds(10));   // 会一直阻塞到 close
        consumer_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(consumer_done.load());
    q.close();
    consumer.join();
    EXPECT_TRUE(consumer_done.load());
    EXPECT_EQ(got, gw::BoundedQueue<int>::PopResult::Closed);
}

TEST(BoundedQueue, CloseAfterDrainReportsClosed) {
    gw::BoundedQueue<int> q(4);
    EXPECT_TRUE(q.push(7));
    q.close();
    EXPECT_FALSE(q.push(8));           // close 后 push 一律失败

    int v = 0;
    EXPECT_EQ(q.wait_pop(v, std::chrono::seconds(1)),
              gw::BoundedQueue<int>::PopResult::Ok);   // 剩余元素先给完
    EXPECT_EQ(v, 7);
    EXPECT_EQ(q.wait_pop(v, std::chrono::seconds(1)),
              gw::BoundedQueue<int>::PopResult::Closed);  // 排空后才 Closed
}

TEST(BoundedQueue, ThreadedOrderingAndNoLoss) {
    // SPSC 契约的核心验证：单生产者 × 单消费者，10 万条不丢不乱
    gw::BoundedQueue<std::uint64_t> q(64);
    constexpr std::uint64_t kCount = 100000;

    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= kCount; ++i) {
            while (!q.push(i)) {
                std::this_thread::yield();   // 队列满：让一让（真实采集线程的退让策略）
            }
        }
        q.close();
    });

    std::uint64_t expect = 1;
    for (;;) {
        std::uint64_t v = 0;
        const auto r = q.wait_pop(v, std::chrono::seconds(5));
        if (r == gw::BoundedQueue<std::uint64_t>::PopResult::Ok) {
            EXPECT_EQ(v, expect);
            ++expect;
            continue;
        }
        break;
    }
    EXPECT_EQ(expect - 1, kCount);
    producer.join();
}

}  // namespace
