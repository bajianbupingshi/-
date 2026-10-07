// 规则引擎（W3-3）单测 —— 表驱动 + 确定性重放 + 与 EdgeProxyService 的集成。
//
// 确定性口径：同一条 (points, ts) 回放流喂两个新引擎，告警序列逐字段一致 ——
// 引擎无随机、时钟由调用方注入，重放是构造保证不是巧合。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest_shim.h"
#include "gw/proxy_service.h"
#include "gw/reliability.h"
#include "gw/rules.h"

namespace {

const char* kValidConfig = R"({
  "rules": [
    {"name": "t-high", "type": "threshold", "addr": 0, "op": "gt", "value": 40000,
     "hysteresis": 500, "cooldown_ms": 1000},
    {"name": "spike", "type": "rate", "addr": 1, "max_delta_per_sec": 1000.0,
     "cooldown_ms": 2000}
  ]
})";

std::vector<gw::Point> pts(std::uint16_t addr, std::uint16_t value) {
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

TEST(Rules, FromJsonValid) {
    const gw::RuleEngine e = gw::RuleEngine::from_json(kValidConfig);
    EXPECT_EQ(e.rule_count(), std::size_t{2});
    ASSERT_EQ(e.threshold_rules().size(), std::size_t{1});
    ASSERT_EQ(e.rate_rules().size(), std::size_t{1});
    EXPECT_EQ(e.threshold_rules()[0].name, "t-high");
    EXPECT_EQ(e.threshold_rules()[0].addr, std::uint16_t{0});
    EXPECT_EQ(e.threshold_rules()[0].op, gw::ThresholdRule::Op::Gt);
    EXPECT_EQ(e.threshold_rules()[0].threshold, std::uint16_t{40000});
    EXPECT_EQ(e.threshold_rules()[0].hysteresis, std::uint16_t{500});
    EXPECT_EQ(e.threshold_rules()[0].cooldown_ms, std::uint64_t{1000});
    EXPECT_EQ(e.rate_rules()[0].max_delta_per_sec, 1000.0);   // 精确可表示，垫片无 DOUBLE_EQ
}

TEST(Rules, FromJsonErrors) {
    struct Case {
        const char* json;
        const char* why;
    };
    const Case cases[] = {
        {"{\"rules\": [{\"name\": \"x\", \"type\": \"magic\", \"addr\": 0}]}", "未知类型"},
        {"{\"rules\": [{\"type\": \"threshold\", \"addr\": 0, \"op\": \"gt\", \"value\": 1}]}",
         "缺 name"},
        {"{\"rules\": [{\"name\": \"x\", \"type\": \"threshold\", \"addr\": 0, \"op\": \"gt\"}]}",
         "缺 value"},
        {"{\"rules\": [{\"name\": \"x\", \"type\": \"threshold\", \"addr\": 0, \"op\": \">>\", \"value\": 1}]}",
         "非法 op"},
        {"{\"rules\": [{\"name\": \"x\", \"type\": \"rate\", \"addr\": 0, \"max_delta_per_sec\": -1}]}",
         "非正变化率"},
        {"{\"rules\": \"not-an-array\"}", "rules 不是数组"},
        {"not json at all", "语法错误"},
    };
    for (const Case& tc : cases) {
        bool threw = false;
        try {
            gw::RuleEngine::from_json(tc.json);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::printf("      [case %s] 应抛 std::runtime_error 而未抛\n", tc.why);
        }
        EXPECT_TRUE(threw);   // 垫片宏不支持 << 流式，失败细节走 printf
    }
}

TEST(Rules, ThresholdHysteresisAndCooldown) {
    gw::RuleEngine e = gw::RuleEngine::from_json(
        "{\"rules\":[{\"name\":\"t\",\"type\":\"threshold\",\"addr\":0,\"op\":\"gt\","
        "\"value\":100,\"hysteresis\":10,\"cooldown_ms\":100}]}");

    // 回放脚本（状态机轨迹，手推 + 逐步断言双保险）：
    //   首样本建基线不告警；越限告警；告警态持续不发；未出迟滞带不发；
    //   出带解除；再越限按冷却判定；冷却窗口内的新告警被吞。
    struct Step {
        std::uint16_t value;
        std::uint64_t ts;
        std::size_t expect_alerts;   // 本步期望产出的告警数
        const char* why;
    };
    const Step steps[] = {
        {50, 0, 0, "首样本建基线"},
        {150, 100, 1, "越限 → 告警①（首次必发）"},
        {160, 200, 0, "告警态持续不发"},
        {95, 300, 0, "未出迟滞带（95 > 100-10=90）"},
        {80, 400, 0, "80 <= 90 → 解除"},
        {150, 450, 1, "再越限 → 告警②（450-100=350 ≥ 冷却100）"},
        {90, 500, 0, "解除"},
        {150, 520, 0, "越限但距告警②仅 70 < 100 ⇒ 冷却吞掉"},
        {90, 600, 0, "解除"},
        {150, 750, 1, "越限 → 告警③（距②650 ≥ 100）"},
    };
    for (const Step& s : steps) {
        const std::vector<gw::Alert> a = e.evaluate(pts(0, s.value), s.ts);
        if (a.size() != s.expect_alerts) {
            std::printf("      [step %s] value=%u ts=%llu 实际告警 %zu 应为 %zu\n",
                        s.why, s.value, static_cast<unsigned long long>(s.ts), a.size(),
                        s.expect_alerts);
        }
        EXPECT_EQ(a.size(), s.expect_alerts);
    }
}

TEST(Rules, RateFiresOnSpikeWithCooldown) {
    gw::RuleEngine e = gw::RuleEngine::from_json(
        "{\"rules\":[{\"name\":\"r\",\"type\":\"rate\",\"addr\":1,"
        "\"max_delta_per_sec\":1000.0,\"cooldown_ms\":2000}]}");

    EXPECT_EQ(e.evaluate(pts(1, 1000), 0).size(), std::size_t{0});      // 基线
    EXPECT_EQ(e.evaluate(pts(1, 1100), 100).size(), std::size_t{0});    // 100/0.1s=1000/s 不超
    EXPECT_EQ(e.evaluate(pts(1, 1500), 200).size(), std::size_t{1});    // 400/0.1s=4000/s ✗ 告警
    EXPECT_EQ(e.evaluate(pts(1, 2900), 300).size(), std::size_t{0});    // 超限但冷却中
    EXPECT_EQ(e.evaluate(pts(1, 3000), 2400).size(), std::size_t{0});   // 100/2.1s 很慢
    EXPECT_EQ(e.evaluate(pts(1, 4500), 2500).size(), std::size_t{1});   // 1500/0.1s，距上次 2300 ≥ 2000 ✓
}

TEST(Rules, DeterministicReplay) {
    // 同一条回放流喂两个新引擎 → 告警序列逐字段一致
    // 回放脚本按状态机手推（阈值 gt 100 迟滞 10 冷却 100；变化率 1000/s 冷却 500）：
    //   阈值告警 @100/400/550/1150（越限→持续→出带解除→再越限……）
    //   变化率告警 @450/1200（520/900 两次超限被冷却吞掉）
    struct Sample {
        std::uint16_t addr;
        std::uint16_t value;
        std::uint64_t ts;
    };
    const std::vector<Sample> stream = {
        {0, 50, 0},     {0, 150, 100},  {0, 160, 200},  {0, 80, 300},
        {1, 1000, 350}, {0, 150, 400},  {1, 1500, 450}, {0, 90, 500},
        {1, 2900, 520}, {0, 150, 550},  {1, 4600, 900}, {0, 80, 1000},
        {0, 150, 1150}, {1, 9000, 1200},
    };
    const char* replay_config =
        "{\"rules\":["
        "{\"name\":\"t-high\",\"type\":\"threshold\",\"addr\":0,\"op\":\"gt\","
        "\"value\":100,\"hysteresis\":10,\"cooldown_ms\":100},"
        "{\"name\":\"spike\",\"type\":\"rate\",\"addr\":1,"
        "\"max_delta_per_sec\":1000.0,\"cooldown_ms\":500}]}";

    const auto run = [&stream, replay_config] {
        gw::RuleEngine e = gw::RuleEngine::from_json(replay_config);
        std::vector<gw::Alert> out;
        for (const Sample& s : stream) {
            for (const gw::Alert& a : e.evaluate(pts(s.addr, s.value), s.ts)) {
                out.push_back(a);
            }
        }
        return out;
    };

    const std::vector<gw::Alert> a = run();
    const std::vector<gw::Alert> b = run();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].rule_name, b[i].rule_name);
        EXPECT_EQ(a[i].kind, b[i].kind);
        EXPECT_EQ(a[i].addr, b[i].addr);
        EXPECT_EQ(a[i].value, b[i].value);
        EXPECT_EQ(a[i].ts_ms, b[i].ts_ms);
        EXPECT_EQ(a[i].detail, b[i].detail);
    }
    // 回放确实触发了两类告警（否则断言在空集上恒真）；手推总量 = 6
    EXPECT_EQ(a.size(), std::size_t{6});
    bool has_threshold = false;
    bool has_rate = false;
    for (const gw::Alert& x : a) {
        has_threshold |= (x.kind == "threshold");
        has_rate |= (x.kind == "rate");
    }
    EXPECT_TRUE(has_threshold);
    EXPECT_TRUE(has_rate);
}

// 本文件自己的记录型传输（test_proxy_service.cpp 里也有一份同名类型，
// 各自在匿名命名空间里内部链接 —— 同名不冲突）。Ledger 非线程安全：
// 管道线程写，测试主线程读，全部经同一把锁（TSan 的既有教训）。
class RecordingTransport : public gw::ITransport {
public:
    explicit RecordingTransport(gw::Ledger& ledger) : ledger_(ledger) {}

    gw::SendResult send(const gw::Record& rec) override {
        std::lock_guard<std::mutex> lk(mtx);
        ledger_.accept(rec);
        return gw::SendResult::Ok;
    }

    std::uint64_t unique_count() const {
        std::lock_guard<std::mutex> lk(mtx);
        return ledger_.unique_count();
    }

private:
    mutable std::mutex mtx;
    gw::Ledger& ledger_;
};

TEST(Rules, IntegrationWithProxyService) {
    // 组装验证：采集线程 produce → 管道线程 end_cycle → 观测钩子 → 规则引擎 → 告警
    gw::Ledger ledger;
    RecordingTransport tx(ledger);
    gw::RecordStore store(100000, 3600000);
    gw::EdgeProxyService svc(store, tx, {}, 0, 1024, 5);

    std::mutex alerts_mtx;
    std::vector<gw::Alert> alerts;   // 管道线程写，测试主线程读 —— 经锁
    gw::RuleEngine engine = gw::RuleEngine::from_json(
        "{\"rules\":[{\"name\":\"t-high\",\"type\":\"threshold\",\"addr\":0,"
        "\"op\":\"gt\",\"value\":30000}]}");

    svc.set_sample_observer([&](const std::vector<gw::Point>& batch, std::uint64_t now_ms) {
        std::vector<gw::Alert> fired = engine.evaluate(batch, now_ms);
        if (!fired.empty()) {
            std::lock_guard<std::mutex> lk(alerts_mtx);
            alerts.insert(alerts.end(), fired.begin(), fired.end());
        }
    });
    svc.start();

    // 阶梯爬升：跨过 30000 阈值 —— 至少一次告警
    for (int i = 0; i <= 400; ++i) {
        svc.produce(pts(0, static_cast<std::uint16_t>(i * 100)));
    }
    EXPECT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> lk(alerts_mtx);
        return !alerts.empty();
    }, std::chrono::seconds(10)));
    svc.stop();

    std::lock_guard<std::mutex> lk(alerts_mtx);
    ASSERT_FALSE(alerts.empty());
    EXPECT_EQ(alerts[0].kind, "threshold");
    EXPECT_EQ(alerts[0].rule_name, "t-high");
    EXPECT_GT(alerts[0].value, 30000);
}

}  // namespace
