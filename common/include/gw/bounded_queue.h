#ifndef GW_BOUNDED_QUEUE_H
#define GW_BOUNDED_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// 有界阻塞队列 —— 多线程代理的采集 → 管道交接件（W3-2）
//
// 设计取舍：
//   · 互斥 + 两个条件变量，而不是无锁原子环 —— 本场景入队频率 = 采样频率
//     （≤ 每秒几百次），锁的开销可忽略；正确性可读性优先。
//     名字不叫 SpscQueue：互斥实现天然多生产者/多消费者安全；
//     但 EdgeProxyService 按 **单生产者单消费者** 契约使用（顺序保证的前提）。
//   · push 不阻塞：队列满返回 false —— 「阻塞采集」还是「丢弃计数」由调用方
//     决策（网关场景两种都要：常规丢并计数，关键路径用 push_wait 阻塞）。
//   · close() 是唯一的关闭信号：唤醒所有等待者；close 后 push 一律失败；
//     消费者把剩余元素排空后收到 Closed。
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : cap_(capacity == 0 ? 1 : capacity) {}

    // 生产端：false = 队列满或已关闭（不阻塞）
    bool push(T&& v) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (closed_ || q_.size() >= cap_) {
                return false;
            }
            q_.push_back(std::move(v));
        }
        cv_pop_.notify_one();
        return true;
    }

    bool push(const T& v) { return push(T(v)); }

    // 生产端阻塞版：队列满时等待，直到有空间或关闭。返回 false = 已关闭。
    bool push_wait(T&& v) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_push_.wait(lk, [&] { return closed_ || q_.size() < cap_; });
        if (closed_) {
            return false;
        }
        q_.push_back(std::move(v));
        lk.unlock();
        cv_pop_.notify_one();
        return true;
    }

    // 消费端三态
    enum class PopResult { Ok, Empty, Closed };

    // 非阻塞尝试
    PopResult try_pop(T& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.empty()) {
            return closed_ ? PopResult::Closed : PopResult::Empty;
        }
        out = std::move(q_.front());
        q_.pop_front();
        cv_push_.notify_one();
        return PopResult::Ok;
    }

    // 阻塞等待至多 timeout：Ok 取到 / Empty 超时且空 / Closed 已关闭（元素排空后）
    PopResult wait_pop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (!cv_pop_.wait_for(lk, timeout, [&] { return !q_.empty() || closed_; })) {
            return PopResult::Empty;
        }
        if (q_.empty()) {
            return PopResult::Closed;   // 关闭且已排空
        }
        out = std::move(q_.front());
        q_.pop_front();
        cv_push_.notify_one();
        return PopResult::Ok;
    }

    // 关闭：唤醒全部等待者；不可逆。调用后 push 一律失败。
    void close() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            closed_ = true;
        }
        cv_pop_.notify_all();
        cv_push_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }

    std::size_t capacity() const noexcept { return cap_; }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_pop_;
    std::condition_variable cv_push_;
    std::deque<T> q_;
    std::size_t cap_;
    bool closed_ = false;
};

}  // namespace gw

#endif  // GW_BOUNDED_QUEUE_H
