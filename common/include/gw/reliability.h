#ifndef GW_RELIABILITY_H
#define GW_RELIABILITY_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <vector>

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// 边缘数据代理的可靠性内核
//
// 目标（方案 §7「可靠性」维度的来源）：
//   「断网 10 分钟恢复后续传丢失 0、重复 0」
//
// 这句话要成立，必须先把三件事分开、各自可验证：
//   ① 序列号：每条记录一个单调递增、**绝不重用**的 seq（去重与对账的唯一依据）
//   ② 落库队列：未确认的记录按 seq 顺序保存，断网期间只累积不丢
//   ③ 状态机：LIVE / BACKFILL / CATCHING_UP 三态显式转换 + 指数退避重连
//
// 另外必须有一个**独立于发送端**的对账器（Ledger）：
// 只让发送端自己说「我发了 1000 条、丢了 0」是不可信的 —— 对账必须站在接收端立场上数。
// 而且判据要有**反转断言**：故意丢包时对账器必须报出丢失，否则「丢失 0」毫无意义。
//
// 依赖全部注入（IClock / ITransport），因此本文件不依赖 SQLite、不依赖 MQTT、
// 不依赖墙上时钟 —— 整条时序可以瞬间跑完且逐位可重放。
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// 投递单元 = 一个采样周期的一批点位（**不是**一个点位一条消息）
//
// ★ 为什么是这个粒度：实测 Paho 同步 API 单条发布 **116 ms**（≈8.6 条/秒，见 README）。
//   若一条消息发一个点位，500 点位 @100ms（=5000 点位/秒）根本不可能。
//   按周期打包后：500 点位 @100ms ⇒ **10 条消息/秒**，剩余量绰绰有余。
//   这也是真实网关的做法 —— Neuron 的 MQTT 北向就是按组发布，而非一点位一消息。
//
//   所以「seq」标识的是**一条消息**，对账时「消息数」与「点位数」是两个独立口径。
// ─────────────────────────────────────────────────────────────────────────────

struct Point {
    std::uint16_t addr = 0;    // 寄存器地址
    std::uint16_t value = 0;   // 值
};

struct Record {
    std::uint64_t seq = 0;              // 单调递增，绝不重用（每条消息一个）
    std::uint64_t ts_ms = 0;            // 采样时刻（来自注入时钟）
    std::vector<Point> points;          // 本周期内的点位（单点场景就是 1 个）

    std::size_t point_count() const noexcept { return points.size(); }
};

// 时钟抽象：不用 std::chrono 的墙钟，保证可重放
class IClock {
public:
    virtual ~IClock() = default;
    virtual std::uint64_t now_ms() const = 0;
};

// 真实时钟实现：测试用注入时钟（SimClock/FakeClock），服务化（W3-2）用这个。
// steady_clock：不受系统时间跳变影响，退避/续传的时间线只认单调流逝。
class SteadyClock final : public IClock {
public:
    std::uint64_t now_ms() const override {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
};

// 发送结果。Ok = 对端已确认（相当于 MQTT QoS1 的 PUBACK）；
// Retry = 链路当前不可用，调用方稍后重试。
enum class SendResult { Ok, Retry };

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual SendResult send(const Record& rec) = 0;
};

// ── 接收端对账器 ────────────────────────────────────────────────────────────
//
// 站在接收侧独立计数：到底收到了什么、有没有重复、有没有缺口。
// 生产环境应换成位图 / 水位线（这里用 set 是为了让测试可读，内存可控即可）。
class Ledger {
public:
    void accept(const Record& rec);   // 允许同一 seq 被投递多次

    std::uint64_t total_received() const noexcept { return total_received_; }
    std::uint64_t unique_count() const noexcept { return seen_.size(); }   // 消息数（去重后）
    std::uint64_t duplicate_count() const noexcept { return duplicates_; }
    std::uint64_t max_seq() const noexcept { return max_seq_; }
    std::uint64_t min_seq() const noexcept { return seen_.empty() ? 0 : *seen_.begin(); }
    bool has(std::uint64_t seq) const { return seen_.count(seq) != 0; }

    // ★ 点位口径：只有**新 seq** 的点位才计入（重复投递的点位不重复累加）
    std::uint64_t points_received() const noexcept { return points_received_; }

    // 缺口统计有两个**语义不同**的口径，别混用：
    //
    // ① missing_within_received()：以「接收端见到的最大 seq」为界，
    //    衡量收到的数据**内部**有没有空洞。用于接收侧自洽性检查。
    //    局限：**从没被发出去的数据它看不见** —— 例如发送端因容量/TTL 丢弃了尾部记录，
    //    接收端的最大 seq 就比生产端小，这个口径仍然会报 0，制造「丢失 0」的假象。
    //
    // ② missing_against_produced(produced_highest_seq)：★ 端到端对账。
    //    「生产了多少、收到多少」，差值就是真丢失。这才是
    //    「断网续传丢失 0」这句结论的**唯一合法依据**。
    std::uint64_t missing_within_received() const;
    std::uint64_t missing_against_produced(std::uint64_t produced_highest_seq) const;

    // 缺口样本（最多 max_out 个），便于打印诊断；以 max_seq 为界
    std::vector<std::uint64_t> missing_sample(std::size_t max_out) const;

private:
    std::set<std::uint64_t> seen_;
    std::uint64_t total_received_ = 0;
    std::uint64_t duplicates_ = 0;
    std::uint64_t max_seq_ = 0;
    std::uint64_t points_received_ = 0;
};

// ── 未确认记录的落库队列 ────────────────────────────────────────────────────
//
// 语义要点：**只保存「未被对端确认」的记录**，ack(seq) 表达「seq 及之前都已确认」。
// 因此 ack 之后才允许回收 —— 这样「按 seq 顺序重放」天然成立。
//
// W3 起有了第二个实现（SQLite 持久化版，store/ 目录）—— 接口先于实现抽出：
// 控制器与代理只认 IRecordStore，内存版（进程内）与 SQLite 版（断电续传）
// 可互换，行为契约由同一组单测钉住。
class IRecordStore {
public:
    virtual ~IRecordStore() = default;

    // 落库。返回 false 表示**因容量上限被丢弃**（dropped 计数 +1，这是真实数据丢失）
    virtual bool push(const Record& rec) = 0;

    // 对端已确认 seq 及之前的全部记录
    virtual void ack(std::uint64_t seq) = 0;

    // TTL 清理：移除超过 TTL 仍未确认的记录并计入 expired。
    // 已确认的早就被 ack() 回收了，所以这里清掉的基本都是「断网太久、再不发就过期」的数据。
    // ⇒ 「断网时长必须小于 TTL」是设计约束：TTL 设小了就一定会丢数据，而且对账器会报出来。
    virtual void expire(std::uint64_t now_ms) = 0;

    virtual std::size_t size() const = 0;
    virtual bool empty() const = 0;
    virtual const Record& front() const = 0;

    virtual std::uint64_t dropped() const = 0;       // 容量超限丢弃
    virtual std::uint64_t expired() const = 0;       // TTL 过期丢弃
    virtual std::uint64_t highest_seq() const = 0;   // 已落库的最高 seq
    virtual std::uint64_t lowest_seq() const = 0;

    // 统计「真正发出去」（被 ack）的条数，用于与对账器交叉核对
    virtual std::uint64_t acked_count() const = 0;

    // seq 水位线钩子：BackfillController 每分配一个 seq 就通知一次。
    // 内存版不需要（进程生共死）；SQLite 版用它持久化「已分配的最高 seq」，
    // 重启后 last_seq_hint() 从这里恢复 —— seq 绝不重用。
    virtual void note_seq(std::uint64_t /*seq*/) {}

    // 重启恢复用：下一次分配应从哪个 seq 之后开始（已分配的最高 seq）。
    // 默认实现退化为 highest_seq()（内存版语义）；持久化版返回持久化的水位线。
    virtual std::uint64_t last_seq_hint() const { return highest_seq(); }
};

class RecordStore : public IRecordStore {
public:
    RecordStore(std::size_t capacity, std::uint64_t ttl_ms);

    // 落库。返回 false 表示**因容量上限被丢弃**（dropped 计数 +1，这是真实数据丢失）
    bool push(const Record& rec) override;

    // 对端已确认 seq 及之前的全部记录
    void ack(std::uint64_t seq) override;

    // TTL 清理：移除超过 TTL 仍未确认的记录并计入 expired。
    // 已确认的早就被 ack() 回收了，所以这里清掉的基本都是「断网太久、再不发就过期」的数据。
    // ⇒ 「断网时长必须小于 TTL」是设计约束：TTL 设小了就一定会丢数据，而且对账器会报出来。
    void expire(std::uint64_t now_ms) override;

    std::size_t size() const override { return queue_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    std::uint64_t ttl_ms() const noexcept { return ttl_ms_; }
    bool empty() const override { return queue_.empty(); }
    const Record& front() const override { return queue_.front(); }

    std::uint64_t dropped() const override { return dropped_; }      // 容量超限丢弃
    std::uint64_t expired() const override { return expired_; }      // TTL 过期丢弃
    std::uint64_t highest_seq() const override { return highest_seq_; }
    std::uint64_t lowest_seq() const override { return queue_.empty() ? 0 : queue_.front().seq; }

    // 统计「真正发出去」（被 ack）的条数，用于与对账器交叉核对
    std::uint64_t acked_count() const override { return acked_count_; }

private:
    std::deque<Record> queue_;   // seq 递增
    std::size_t capacity_;
    std::uint64_t ttl_ms_;
    std::uint64_t dropped_ = 0;
    std::uint64_t expired_ = 0;
    std::uint64_t highest_seq_ = 0;
    std::uint64_t acked_count_ = 0;
};

// ── 续传状态机 ──────────────────────────────────────────────────────────────
//
// 三态定义（方案 §5③ 提到的 LIVE / CATCHING_UP / BACKFILL）：
//   Live       链路正常且无积压：新样本即时发送
//   Backfill   链路不可用：样本只落库；按指数退避重连（「待回填」）
//   CatchingUp 链路已恢复但仍有积压：按 seq 顺序排空；排空后回到 Live
//
// 状态不是外部设置进去的，而是由「上次发送结果 + 积压是否为空」推导出来的 ——
// 这样不存在「状态与事实不一致」的可能。
enum class LinkState { Live, Backfill, CatchingUp };

struct BackfillConfig {
    std::uint32_t backoff_initial_ms = 500;
    std::uint32_t backoff_max_ms = 30000;
    std::uint32_t backoff_multiplier = 2;
    std::size_t drain_batch = 256;   // 单次 tick 最多排空多少条
};

struct LinkStats {
    std::uint64_t produced = 0;      // 封出的消息数
    std::uint64_t points = 0;        // 封出的点位数合计
    std::uint64_t sent = 0;          // 成功发送次数（含续传重发）
    std::uint64_t retries = 0;       // 发送返回 Retry 的次数
    std::uint64_t reconnects = 0;    // Backfill → CatchingUp 次数
    std::uint64_t backlog_peak = 0;  // 积压峰值（消息数）
    std::uint64_t max_backoff_ms = 0;
    std::uint64_t backoff_waits = 0; // 因退避而推迟重试的次数
};

class BackfillController {
public:
    // seq_start：重启恢复用 —— 传入 IRecordStore::last_seq_hint()（持久化水位线），
    // 让 seq 从断点之后继续分配，绝不重用。新进程不传（= 0，从 1 开始）。
    BackfillController(IRecordStore& store, ITransport& transport, IClock& clock,
                       BackfillConfig config = BackfillConfig{},
                       std::uint64_t seq_start = 0);

    // 采集到一个样本：分配 seq 由调用方给（或见 next_seq()），落库并按状态发送
    void on_sample(const Record& rec);

    // 周期驱动：推进重连退避与积压排空
    void on_tick();

    LinkState state() const noexcept { return state_; }
    const LinkStats& stats() const noexcept { return stats_; }

    // 单调序列号分配器（绝不重用；重启后由 seq_start 从持久化水位线恢复）
    std::uint64_t next_seq() noexcept {
        ++seq_;
        store_.note_seq(seq_);   // 持久化版借此落水位线；内存版是空操作
        return seq_;
    }
    std::uint64_t last_allocated_seq() const noexcept { return seq_; }

private:
    void try_send_front();
    void drain_batch_once();
    void enter_backfill();
    void enter_catching_up();
    void enter_live();

    IRecordStore& store_;
    ITransport& transport_;
    IClock& clock_;
    BackfillConfig cfg_;

    LinkState state_ = LinkState::Live;
    bool link_ok_ = true;
    std::uint64_t next_retry_at_ms_ = 0;
    std::uint32_t backoff_ms_ = 0;
    std::uint64_t seq_ = 0;
    LinkStats stats_{};
};

// ── 便捷组合：把「采样 → 落库 → 续传」串起来，便于测试与复用 ────────────────
struct ProxyConfig {
    std::size_t store_capacity = 100000;
    std::uint64_t store_ttl_ms = 3600000;   // 默认 1 小时 —— 必须大于最长断网时间
    // 单条消息最多携带多少点位。超过就拆成多条 ——
    // 这条不变式保证「每个 Record 都是可编码的」（编码器有上限）。
    std::size_t max_points_per_message = 1024;
    BackfillConfig backfill{};
};

class DataProxy {
public:
    DataProxy(IRecordStore& store, ITransport& transport, IClock& clock,
              ProxyConfig config = ProxyConfig{}, std::uint64_t seq_start = 0);

    // ── 单点模式（兼容/旧口径）：一个点位就是一条消息 ──────────────────────
    void sample(std::uint16_t addr, std::uint16_t value);
    void tick();

    // ── 批量模式（推荐）：一个采样周期内的多个点位封成**一条**消息 ──────────
    //
    //   用法：每周期对 N 个点位调 add_point()，周期末调 end_cycle() 封包。
    //   返回本批点位数；批次为空时 end_cycle() 不产生消息。
    void add_point(std::uint16_t addr, std::uint16_t value);
    std::size_t end_cycle();
    std::size_t pending_points() const noexcept { return pending_.size(); }

    LinkState state() const noexcept { return ctrl_.state(); }
    const LinkStats& stats() const noexcept { return ctrl_.stats(); }
    const IRecordStore& store() const noexcept { return store_; }
    // 已分配的最高 seq —— 端到端对账必须拿它去和接收端比，不能用接收端的最大 seq
    std::uint64_t produced_highest_seq() const noexcept { return ctrl_.last_allocated_seq(); }

private:
    IRecordStore& store_;
    BackfillController ctrl_;
    IClock& clock_;
    ProxyConfig cfg_;
    std::vector<Point> pending_;
};

}  // namespace gw

#endif  // GW_RELIABILITY_H
