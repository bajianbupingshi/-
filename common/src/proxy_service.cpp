#include "gw/proxy_service.h"

#include <algorithm>

namespace gw {

EdgeProxyService::EdgeProxyService(IRecordStore& store, ITransport& transport, ProxyConfig cfg,
                                   std::uint64_t seq_start, std::size_t queue_capacity,
                                   std::uint32_t tick_ms)
    : queue_(queue_capacity),
      tick_ms_(tick_ms == 0 ? 1 : tick_ms),
      steady_clock_(),
      proxy_(store, transport, steady_clock_, cfg, seq_start) {}

EdgeProxyService::~EdgeProxyService() {
    stop();
}

void EdgeProxyService::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;   // 已启动
    }
    worker_ = std::thread([this] { pipeline_loop(); });
}

void EdgeProxyService::stop() noexcept {
    if (!running_.exchange(false)) {
        return;   // 未启动或已停
    }
    queue_.close();   // 唤醒管道；它把剩余批次排空后退出
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool EdgeProxyService::produce(std::vector<Point>&& points) {
    if (!queue_.push(std::move(points))) {
        if (!queue_.closed()) {
            // 队列满：采集侧的真实丢弃，必须可见（与 store 容量丢弃是两个口径）
            ++queued_dropped_;
        }
        return false;
    }
    return true;
}

LinkStats EdgeProxyService::stats() const {
    std::lock_guard<std::mutex> lk(obs_mtx_);
    return stats_;
}

void EdgeProxyService::pipeline_loop() {
    std::vector<Point> batch;
    for (;;) {
        // 等新批（至多一个节拍）—— 无数据也要醒，驱动退避/过期/排空
        BoundedQueue<std::vector<Point>>::PopResult r =
            queue_.wait_pop(batch, std::chrono::milliseconds(tick_ms_));
        bool have_batch = false;
        if (r == BoundedQueue<std::vector<Point>>::PopResult::Ok) {
            have_batch = true;
        } else if (r == BoundedQueue<std::vector<Point>>::PopResult::Closed) {
            // 排空剩余批次后退出（优雅停止不丢已收的）
            have_batch = queue_.try_pop(batch) ==
                         BoundedQueue<std::vector<Point>>::PopResult::Ok;
            if (!have_batch) {
                break;
            }
        }

        // 内核与观测钩子全程 try 包裹：SQLite 损坏这类故障停管道、观测面可见，
        // 不静默吞；观测钩子按契约不得抛，这里兜底是纵深防御
        try {
            if (have_batch) {
                for (const Point& p : batch) {
                    proxy_.add_point(p.addr, p.value);
                }
                // 一批 = 一条消息；seq 在此分配（note_seq 落水位线）
                if (proxy_.end_cycle() > 0 && observer_) {
                    observer_(batch, steady_clock_.now_ms());
                }
            }
            // Empty / 处理完一批：都要驱动一次内核 tick（退避/过期/排空的时间线）
            proxy_.tick();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(obs_mtx_);
            last_error_ = e.what();
            break;   // 内核异常（如 SQLite 损坏）：停管道，观测面可见，不静默吞
        } catch (...) {
            std::lock_guard<std::mutex> lk(obs_mtx_);
            last_error_ = "pipeline: unknown exception";
            break;
        }
        state_mirror_.store(static_cast<int>(proxy_.state()), std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(obs_mtx_);
            stats_ = proxy_.stats();
        }
    }
    state_mirror_.store(static_cast<int>(proxy_.state()), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(obs_mtx_);
        stats_ = proxy_.stats();
    }
    running_.store(false, std::memory_order_relaxed);
}

}  // namespace gw
