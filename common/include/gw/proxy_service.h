#ifndef GW_PROXY_SERVICE_H
#define GW_PROXY_SERVICE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gw/bounded_queue.h"
#include "gw/reliability.h"

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// 边缘数据代理 · 多线程服务壳（W3-2）
//
// 线程模型（actor / 管道）：
//
//   采集线程（任意）                    管道线程（唯一）                Paho 线程（库内）
//   ───────────────                    ────────────────              ─────────────
//   produce(points批) ──► BoundedQueue ──► DataProxy / RecordStore ──► Async 发送
//                          （互斥+cv）      ITransport / 控制器          回调 ──► cv
//
//   · 内核（DataProxy/Store/控制器/传输）**只在管道线程上被触碰** —— 已验证的
//     单线程语义零改动、零锁零竞态；多线程的全部同步都发生在边界（BoundedQueue
//     与 Paho 回调的完成槽），这两处正是 TSan 的检查对象。
//   · 采集线程永不触碰网络/磁盘：SQLite 落库、PUBACK 等待都发生在管道线程。
//   · stop() 的契约：不再收新批 → 管道把队列**排空**（优雅排空，不丢已收的）
//     → 线程退出。store 里未确认的记录原样保留（持久化版的断电续传语义）。
// ─────────────────────────────────────────────────────────────────────────────
class EdgeProxyService {
public:
    // queue_capacity：采集与管道之间的交接队列上限（满 ⇒ produce 返回 false 并计数）
    // tick_ms：管道节拍 —— 无新数据时也按此周期驱动一次内核 tick（退避/过期/排空）
    explicit EdgeProxyService(IRecordStore& store, ITransport& transport,
                              ProxyConfig cfg = ProxyConfig{}, std::uint64_t seq_start = 0,
                              std::size_t queue_capacity = 65536, std::uint32_t tick_ms = 10);
    ~EdgeProxyService();

    EdgeProxyService(const EdgeProxyService&) = delete;
    EdgeProxyService& operator=(const EdgeProxyService&) = delete;

    void start();
    // 优雅停止：关闭交接队列，等管道排空剩余批次后退出（阻塞直至 join）
    void stop() noexcept;

    // 采集线程 API：交出「一个采样周期的点位批」。seq 由管道线程分配（保序）。
    // 返回 false = 队列满（已计 queued_dropped）或服务已停。
    bool produce(std::vector<Point>&& points);

    // 观测（多线程安全：管道线程每拍镜像快照）
    LinkState state() const noexcept { return static_cast<LinkState>(state_mirror_.load(std::memory_order_relaxed)); }
    LinkStats stats() const;
    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
    std::uint64_t queued_dropped() const noexcept {
        return queued_dropped_.load(std::memory_order_relaxed);
    }
    std::size_t queued() const noexcept { return queue_.size(); }
    const std::string& last_error() const noexcept { return last_error_; }

private:
    void pipeline_loop();

    // 声明顺序即初始化顺序：时钟必须先于 proxy_（后者持其引用）
    BoundedQueue<std::vector<Point>> queue_;
    std::uint32_t tick_ms_;
    SteadyClock steady_clock_;

    // 内核成员 —— 只在管道线程触碰
    DataProxy proxy_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> queued_dropped_{0};
    std::atomic<int> state_mirror_{0};   // LinkState

    mutable std::mutex obs_mtx_;
    LinkStats stats_{};
    std::string last_error_;
};

}  // namespace gw

#endif  // GW_PROXY_SERVICE_H
