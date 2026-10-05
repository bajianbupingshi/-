// 边缘数据代理 · 断网续传场景复现器
//
// 为什么单独做成一个可执行程序（而不是只留在单测里）：
//   方案 §7 的「可靠性」指标要能**独立复现并出图**，也要能在面试现场当场演示。
//   全部时序用注入时钟跑，所以「断网 10 分钟」是瞬间跑完的，不是真的等 10 分钟。
//
// 用法：
//   proxy_demo                                    # 默认：30 分钟时长，第 5 分钟起断网 10 分钟
//   proxy_demo --outage-min 10 --period-ms 250 --total-min 30
//   proxy_demo --drop-every 97                    # 反转演示：制造静默丢包，看对账器能否发现
//   proxy_demo --capacity 100                     # 演示容量打满会真的丢数据
//   proxy_demo --ttl-min 1                        # 演示 TTL 小于断网时长会真的丢数据
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gw/reliability.h"

namespace {

class SimClock : public gw::IClock {
public:
    std::uint64_t now_ms() const override { return now_ms_; }
    void advance(std::uint64_t ms) { now_ms_ += ms; }

private:
    std::uint64_t now_ms_ = 0;
};

// 可编程传输。drop_every_n > 0 时，每 n 次「成功」里静默丢 1 条
//（对端没收到但返回成功）—— 这是最危险的故障，也是反转断言的靶子。
class SimTransport : public gw::ITransport {
public:
    explicit SimTransport(gw::Ledger& ledger) : ledger_(ledger) {}

    gw::SendResult send(const gw::Record& rec) override {
        ++attempts;
        if (down) {
            return gw::SendResult::Retry;
        }
        if (drop_every_n > 0 && (++ok_ % drop_every_n) == 0) {
            ++silently_dropped;
            return gw::SendResult::Ok;
        }
        ledger_.accept(rec);
        return gw::SendResult::Ok;
    }

    bool down = false;
    std::uint64_t drop_every_n = 0;
    std::uint64_t attempts = 0;
    std::uint64_t silently_dropped = 0;

private:
    gw::Ledger& ledger_;
    std::uint64_t ok_ = 0;
};

struct Options {
    std::uint64_t period_ms = 250;
    std::uint64_t total_min = 30;
    std::uint64_t outage_start_min = 5;
    std::uint64_t outage_min = 10;
    std::size_t capacity = 20000;
    std::uint64_t ttl_min = 60;
    std::uint64_t drop_every = 0;
    std::uint64_t drain_batch = 256;
};

const char* state_name(gw::LinkState s) {
    switch (s) {
        case gw::LinkState::Live:       return "Live";
        case gw::LinkState::Backfill:   return "Backfill";
        case gw::LinkState::CatchingUp: return "CatchingUp";
    }
    return "?";
}

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("    [FAIL] %s\n", what.c_str());
    }
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](const char* n) -> const char* {
            if (i + 1 >= argc) {
                std::printf("%s 缺参数\n", n);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--period-ms") {
            o.period_ms = std::strtoull(val("--period-ms"), nullptr, 10);
        } else if (a == "--total-min") {
            o.total_min = std::strtoull(val("--total-min"), nullptr, 10);
        } else if (a == "--outage-start-min") {
            o.outage_start_min = std::strtoull(val("--outage-start-min"), nullptr, 10);
        } else if (a == "--outage-min") {
            o.outage_min = std::strtoull(val("--outage-min"), nullptr, 10);
        } else if (a == "--capacity") {
            o.capacity = std::strtoul(val("--capacity"), nullptr, 10);
        } else if (a == "--ttl-min") {
            o.ttl_min = std::strtoull(val("--ttl-min"), nullptr, 10);
        } else if (a == "--drop-every") {
            o.drop_every = std::strtoull(val("--drop-every"), nullptr, 10);
        } else if (a == "--drain-batch") {
            o.drain_batch = std::strtoull(val("--drain-batch"), nullptr, 10);
        } else if (a == "--help" || a == "-h") {
            std::printf("用法: proxy_demo [--period-ms N] [--total-min N] [--outage-start-min N]\n"
                        "                  [--outage-min N] [--capacity N] [--ttl-min N]\n"
                        "                  [--drop-every N] [--drain-batch N]\n");
            std::exit(0);
        } else {
            std::printf("未知选项 %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        return 2;
    }

    const std::uint64_t total_ms = opt.total_min * 60 * 1000;
    const std::uint64_t outage_at = opt.outage_start_min * 60 * 1000;
    const std::uint64_t outage_until = outage_at + opt.outage_min * 60 * 1000;

    SimClock clock;
    gw::Ledger ledger;
    SimTransport tx(ledger);
    tx.drop_every_n = opt.drop_every;

    gw::ProxyConfig cfg;
    cfg.store_capacity = opt.capacity;
    cfg.store_ttl_ms = opt.ttl_min * 60 * 1000;
    cfg.backfill.drain_batch = opt.drain_batch;

    gw::RecordStore store(cfg.store_capacity, cfg.store_ttl_ms);
    gw::DataProxy proxy(store, tx, clock, cfg);

    std::printf("=========== 边缘数据代理 · 断网续传场景 ===========\n");
    std::printf("  采样周期      : %llu ms\n", static_cast<unsigned long long>(opt.period_ms));
    std::printf("  总时长        : %llu 分钟\n", static_cast<unsigned long long>(opt.total_min));
    std::printf("  断网          : 第 %llu 分钟起，持续 %llu 分钟\n",
                static_cast<unsigned long long>(opt.outage_start_min),
                static_cast<unsigned long long>(opt.outage_min));
    std::printf("  队列容量 / TTL: %zu 条 / %llu 分钟\n", opt.capacity,
                static_cast<unsigned long long>(opt.ttl_min));
    if (opt.drop_every > 0) {
        std::printf("  故障注入      : 每 %llu 次成功投递中静默丢 1 条（反转断言用）\n",
                    static_cast<unsigned long long>(opt.drop_every));
    }
    std::printf("\n时间轴（仅在状态变化时打印）:\n");

    gw::LinkState prev = proxy.state();
    std::printf("  t=%6.1f min  %-11s 产出 %-6llu 积压 %zu\n", 0.0, state_name(prev),
                static_cast<unsigned long long>(proxy.stats().produced), store.size());

    bool outage_done = false;
    for (std::uint64_t t = 0; t < total_ms; t += opt.period_ms) {
        if (t == outage_at) {
            tx.down = true;
        }
        if (!outage_done && t >= outage_until) {
            tx.down = false;
            outage_done = true;
        }
        proxy.sample(0, static_cast<std::uint16_t>(t / opt.period_ms));
        clock.advance(opt.period_ms);
        proxy.tick();

        if (proxy.state() != prev) {
            std::printf("  t=%6.1f min  %-11s 产出 %-6llu 积压 %zu\n",
                        static_cast<double>(t) / 60000.0, state_name(proxy.state()),
                        static_cast<unsigned long long>(proxy.stats().produced), store.size());
            prev = proxy.state();
        }
    }

    const std::uint64_t produced = proxy.stats().produced;
    const std::uint64_t produced_seq = proxy.produced_highest_seq();
    const std::uint64_t unique = ledger.unique_count();
    const std::uint64_t dup = ledger.duplicate_count();
    const std::uint64_t e2e_missing = ledger.missing_against_produced(produced_seq);
    const std::uint64_t inner_missing = ledger.missing_within_received();

    std::printf("\n结果（口径：端到端对账 = 生产端最高 seq %llu − 接收端去重条数）:\n",
                static_cast<unsigned long long>(produced_seq));
    std::printf("  产出            : %llu 条\n", static_cast<unsigned long long>(produced));
    std::printf("  接收去重        : %llu 条（重复投递 %llu 次）\n",
                static_cast<unsigned long long>(unique), static_cast<unsigned long long>(dup));
    std::printf("  ★ 端到端丢失     : %llu 条\n", static_cast<unsigned long long>(e2e_missing));
    std::printf("    接收侧内部缺口 : %llu 条（口径不同，别混用）\n",
                static_cast<unsigned long long>(inner_missing));
    std::printf("  积压峰值        : %llu 条\n",
                static_cast<unsigned long long>(proxy.stats().backlog_peak));
    std::printf("  重连次数        : %llu（退避上限 %llu ms）\n",
                static_cast<unsigned long long>(proxy.stats().reconnects),
                static_cast<unsigned long long>(proxy.stats().max_backoff_ms));
    std::printf("  容量丢弃 / TTL 过期: %llu / %llu 条\n",
                static_cast<unsigned long long>(store.dropped()),
                static_cast<unsigned long long>(store.expired()));
    std::printf("  结束时状态      : %s（积压 %zu 条）\n", state_name(proxy.state()), store.size());

    // ── 判据 ────────────────────────────────────────────────────────────────
    std::printf("\n判据:\n");
    check(produced > 0, "应当有产出");
    if (opt.drop_every == 0 && store.dropped() == 0 && store.expired() == 0) {
        check(e2e_missing == 0, "★ 端到端丢失必须为 0");
        check(dup == 0, "★ 重复投递必须为 0");
        check(unique == produced, "接收去重条数应等于产出条数");
    } else {
        check(e2e_missing > 0, "★ 反转断言：存在真实丢弃时，对账器必须报出丢失");
    }
    check(store.size() == 0, "结束时积压应排空");
    check(proxy.state() == gw::LinkState::Live, "结束时状态应为 Live");

    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
