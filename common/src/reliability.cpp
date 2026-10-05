#include "gw/reliability.h"

#include <algorithm>

namespace gw {

// ── Ledger ──────────────────────────────────────────────────────────────────
void Ledger::accept(const Record& rec) {
    ++total_received_;
    const auto inserted = seen_.insert(rec.seq);
    if (!inserted.second) {
        ++duplicates_;
        return;   // 重复投递：点位不重复累加
    }
    points_received_ += rec.point_count();
    if (rec.seq > max_seq_) {
        max_seq_ = rec.seq;
    }
}

std::uint64_t Ledger::missing_within_received() const {
    // seq 约定从 1 开始且连续 ⇒ [1, max_seq] 内的缺口数 = max_seq - 已见去重数
    return max_seq_ >= seen_.size() ? max_seq_ - static_cast<std::uint64_t>(seen_.size()) : 0;
}

std::uint64_t Ledger::missing_against_produced(std::uint64_t produced_highest_seq) const {
    // 端到端对账：生产端最高 seq 减去接收端去重后的条数。
    // 前提：seq 从 1 开始连续分配（由 BackfillController::next_seq() 保证）。
    return produced_highest_seq >= seen_.size()
               ? produced_highest_seq - static_cast<std::uint64_t>(seen_.size())
               : 0;
}

std::vector<std::uint64_t> Ledger::missing_sample(std::size_t max_out) const {
    std::vector<std::uint64_t> out;
    for (std::uint64_t s = 1; s <= max_seq_ && out.size() < max_out; ++s) {
        if (seen_.count(s) == 0) {
            out.push_back(s);
        }
    }
    return out;
}

// ── RecordStore ─────────────────────────────────────────────────────────────
RecordStore::RecordStore(std::size_t capacity, std::uint64_t ttl_ms)
    : capacity_(capacity == 0 ? 1 : capacity), ttl_ms_(ttl_ms) {}

bool RecordStore::push(const Record& rec) {
    if (queue_.size() >= capacity_) {
        // 容量打满：拒收最新的一条并计数。
        // 丢「最新」而不是「最老」，是为了让队列头部的 seq 保持连续 ——
        // 接收端看到的是一个尾部缺口，而不是一个中间空洞（更容易定位）。
        ++dropped_;
        return false;
    }
    if (rec.seq > highest_seq_) {
        highest_seq_ = rec.seq;
    }
    queue_.push_back(rec);
    return true;
}

void RecordStore::ack(std::uint64_t seq) {
    while (!queue_.empty() && queue_.front().seq <= seq) {
        queue_.pop_front();
        ++acked_count_;
    }
}

void RecordStore::expire(std::uint64_t now_ms) {
    while (!queue_.empty()) {
        const Record& front = queue_.front();
        if (now_ms < front.ts_ms || now_ms - front.ts_ms <= ttl_ms_) {
            break;  // 队列按 seq/时间递增，后面只会更年轻
        }
        queue_.pop_front();
        ++expired_;
    }
}

// ── BackfillController ──────────────────────────────────────────────────────
BackfillController::BackfillController(RecordStore& store, ITransport& transport, IClock& clock,
                                       BackfillConfig config)
    : store_(store),
      transport_(transport),
      clock_(clock),
      cfg_(config),
      backoff_ms_(config.backoff_initial_ms) {}

void BackfillController::enter_backfill() {
    const std::uint64_t now = clock_.now_ms();
    if (state_ != LinkState::Backfill) {
        state_ = LinkState::Backfill;
        backoff_ms_ = cfg_.backoff_initial_ms;
    } else {
        // 已在 Backfill 且又失败一次 ⇒ 指数退避
        const std::uint64_t grown = static_cast<std::uint64_t>(backoff_ms_) * cfg_.backoff_multiplier;
        backoff_ms_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(grown, cfg_.backoff_max_ms));
    }
    next_retry_at_ms_ = now + backoff_ms_;
    ++stats_.backoff_waits;
    stats_.max_backoff_ms = std::max<std::uint64_t>(stats_.max_backoff_ms, backoff_ms_);
}

void BackfillController::enter_catching_up() {
    state_ = LinkState::CatchingUp;
}

void BackfillController::enter_live() {
    state_ = LinkState::Live;
}

void BackfillController::try_send_front() {
    if (store_.empty()) {
        if (link_ok_ && state_ != LinkState::Live) {
            enter_live();
        }
        return;
    }

    const Record rec = store_.front();
    if (transport_.send(rec) == SendResult::Ok) {
        ++stats_.sent;
        store_.ack(rec.seq);
        const bool was_down = !link_ok_;
        link_ok_ = true;
        backoff_ms_ = cfg_.backoff_initial_ms;
        if (was_down) {
            ++stats_.reconnects;
            enter_catching_up();
        }
        if (store_.empty()) {
            enter_live();
        }
    } else {
        ++stats_.retries;
        link_ok_ = false;
        enter_backfill();
    }
}

void BackfillController::drain_batch_once() {
    std::size_t served = 0;
    while (served < cfg_.drain_batch && !store_.empty() && state_ == LinkState::CatchingUp) {
        try_send_front();
        ++served;
        if (!link_ok_) {
            break;  // 排空途中再次失败 → 已进入 Backfill
        }
    }
}

void BackfillController::on_sample(const Record& rec) {
    ++stats_.produced;
    stats_.points += rec.point_count();
    if (!store_.push(rec)) {
        return;  // 容量超限：已计入 store.dropped()，这是真实数据丢失
    }
    stats_.backlog_peak = std::max<std::uint64_t>(stats_.backlog_peak, store_.size());
    if (state_ == LinkState::Live) {
        try_send_front();
    }
}

void BackfillController::on_tick() {
    store_.expire(clock_.now_ms());

    switch (state_) {
        case LinkState::Backfill: {
            if (clock_.now_ms() < next_retry_at_ms_) {
                return;  // 还在退避窗口内
            }
            try_send_front();
            if (state_ == LinkState::CatchingUp) {
                drain_batch_once();
            }
            break;
        }
        case LinkState::CatchingUp:
            drain_batch_once();
            break;
        case LinkState::Live:
            if (!store_.empty()) {  // 防御：Live 却还有积压
                try_send_front();
            }
            break;
    }
}

// ── DataProxy ───────────────────────────────────────────────────────────────
DataProxy::DataProxy(RecordStore& store, ITransport& transport, IClock& clock, ProxyConfig config)
    : store_(store),
      ctrl_(store, transport, clock, config.backfill),
      clock_(clock),
      cfg_(config) {}

void DataProxy::sample(std::uint16_t addr, std::uint16_t value) {
    Record rec;
    rec.seq = ctrl_.next_seq();
    rec.ts_ms = clock_.now_ms();
    rec.points.push_back(Point{addr, value});
    ctrl_.on_sample(rec);
}

void DataProxy::add_point(std::uint16_t addr, std::uint16_t value) {
    pending_.push_back(Point{addr, value});
}

std::size_t DataProxy::end_cycle() {
    if (pending_.empty()) {
        return 0;   // 空周期不产生消息
    }
    const std::size_t cap = cfg_.max_points_per_message == 0 ? 1 : cfg_.max_points_per_message;
    const std::size_t total = pending_.size();
    std::size_t emitted = 0;

    // 点位超过单条上限就拆成多条（保证每个 Record 都能被编码器接受）
    for (std::size_t off = 0; off < total; off += cap) {
        const std::size_t n = std::min(cap, total - off);
        Record rec;
        rec.seq = ctrl_.next_seq();
        rec.ts_ms = clock_.now_ms();
        rec.points.assign(pending_.begin() + static_cast<std::ptrdiff_t>(off),
                          pending_.begin() + static_cast<std::ptrdiff_t>(off + n));
        ctrl_.on_sample(rec);
        emitted += n;
    }
    pending_.clear();
    return emitted;
}

void DataProxy::tick() {
    ctrl_.on_tick();
}

}  // namespace gw
