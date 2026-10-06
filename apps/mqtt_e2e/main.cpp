// MQTT 端到端：让「断网续传」跑在真 MQTT 上
//
// 三种角色（同一个可执行程序）：
//   --selftest                     进程内起最小 broker + 代理，脚本化制造断网并自动对账（CI 用）
//   --broker [--port N]            单独跑一个最小 broker（可跨进程对账；也能给 MQTTX 连）
//   --pub    --host H --port P     发布侧：DataProxy + MqttTransport
//   --sub    --host H --port P     订阅侧：Paho 订阅并把收到的 seq 喂给对账器
//
// 为什么 broker 自己要做接收端对账：
//   「丢失 0」只能站在**接收端**数。让发布端自己报「我发了 N 条都成功」是不可信的。
//   --broker 模式把这件事跨进程化：A 窗口跑 broker（数唯一/缺失/重复），B 窗口跑 --pub。
//   对着真 broker（NanoMQ/Mosquitto）则用 --sub 来充当接收端。
//
// 真实断网的模拟：--selftest 用 broker.stop()（关监听 + 断所有连接）再 start()；
//   --pub 用 --outage-sec（到点主动断开自己的连接，等价于链路 down）。
// ASIO_STANDALONE 由 CMake 传入；这里兜底（-Werror 会抓重复定义）
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "gw/mqtt_transport.h"
#include "gw/reliability.h"
#include "gw/testing/mini_mqtt_broker.h"

// Paho 只用在这里的订阅侧与信号处理上（发布侧的传输层在 gw/mqtt 里）
extern "C" {
#include "MQTTAsync.h"
}

#include <condition_variable>
#include <mutex>

namespace {

// ── 注入时钟：让「断网 N 秒」瞬间跑完 ────────────────────────────────────────
class SimClock : public gw::IClock {
public:
    std::uint64_t now_ms() const override { return now_; }
    void advance(std::uint64_t ms) { now_ += ms; }

private:
    std::uint64_t now_ = 0;
};

struct Options {
    std::string role;
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string topic = "neuron/gateway/data";
    std::uint64_t count = 0;                // 采样周期数；0 = 用角色默认值
    std::uint64_t points_per_cycle = 0;     // 每周期点位数；0 = 用角色默认值（>1 即批量打包）
    std::uint64_t period_ms = 100;
    std::uint64_t outage_sec = 0;
    std::uint64_t expect = 0;      // --sub / --broker 用：期望的生产端最高 seq
    std::uint64_t expect_points = 0;        // 期望的点位数合计
};

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("    [FAIL] %s\n", what.c_str());
    }
}

void usage() {
    std::printf(
        "用法:\n"
        "  mqtt_e2e --selftest [--count 周期数] [--points 每周期点位数]\n"
        "  mqtt_e2e --bench [--count 总点位数] [--points 批量大小]      打包前后吞吐对比\n"
        "  mqtt_e2e --broker [--port 1884] [--expect 消息数] [--expect-points N]\n"
        "  mqtt_e2e --pub --host H --port P [--count 周期] [--points N] [--period-ms M] [--outage-sec S]\n"
        "  mqtt_e2e --sub --host H --port P [--expect 消息数] [--expect-points N] [--topic T]\n"
        "  mqtt_e2e --rawsub --host H --port P --topic T [--count N] [--outage-sec 秒]   打印原始报文\n");
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
        if (a == "--selftest") {
            o.role = "selftest";
        } else if (a == "--bench") {
            o.role = "bench";
        } else if (a == "--broker") {
            o.role = "broker";
        } else if (a == "--pub") {
            o.role = "pub";
        } else if (a == "--sub") {
            o.role = "sub";
        } else if (a == "--rawsub") {
            o.role = "rawsub";
        } else if (a == "--host") {
            o.host = val("--host");
        } else if (a == "--port") {
            o.port = static_cast<std::uint16_t>(std::strtoul(val("--port"), nullptr, 10));
        } else if (a == "--topic") {
            o.topic = val("--topic");
        } else if (a == "--count") {
            o.count = std::strtoull(val("--count"), nullptr, 10);
        } else if (a == "--points") {
            o.points_per_cycle = std::strtoull(val("--points"), nullptr, 10);
        } else if (a == "--expect-points") {
            o.expect_points = std::strtoull(val("--expect-points"), nullptr, 10);
        } else if (a == "--period-ms") {
            o.period_ms = std::strtoull(val("--period-ms"), nullptr, 10);
        } else if (a == "--outage-sec") {
            o.outage_sec = std::strtoull(val("--outage-sec"), nullptr, 10);
        } else if (a == "--expect") {
            o.expect = std::strtoull(val("--expect"), nullptr, 10);
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            std::printf("未知选项 %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

gw::MqttConfig make_cfg(const Options& o, const std::string& client_id) {
    gw::MqttConfig cfg;
    cfg.host = o.host;
    cfg.port = o.port;
    cfg.client_id = client_id;
    cfg.topic = o.topic;
    cfg.qos = 1;
    cfg.ack_timeout_ms = 1500;
    return cfg;
}

// ── broker 角色 ─────────────────────────────────────────────────────────────
int run_broker(const Options& o) {
    asio::io_context io;
    gw::testing::MiniMqttBroker broker(io);
    const std::uint16_t want = o.port == 0 ? 1884 : o.port;
    if (!broker.start(want)) {
        std::printf("broker 启动失败（端口 %u 被占？）\n", want);
        return 1;
    }
    std::printf("最小 MQTT broker 监听 0.0.0.0:%u（Ctrl-C 退出）\n", broker.port());
    if (o.expect > 0) {
        std::printf("将在收到 %llu 条（生产端最高 seq）后自动对账并退出\n",
                    static_cast<unsigned long long>(o.expect));
    }
    std::printf("  订阅主题建议: %s\n", o.topic.c_str());

    std::thread worker([&io] { io.run(); });
    auto work_guard = asio::make_work_guard(io);
    bool done = false;
    std::uint64_t last = 0;
    int quiet_rounds = 0;
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::uint64_t got = broker.ledger().unique_count();
        if (got != last) {
            last = got;
            quiet_rounds = 0;
            std::printf("  已收到 %llu 条（重复投递 %llu / 载荷非法 %llu）\n",
                        static_cast<unsigned long long>(got),
                        static_cast<unsigned long long>(broker.duplicate_publishes()),
                        static_cast<unsigned long long>(broker.bad_payloads()));
            std::fflush(stdout);
        } else {
            ++quiet_rounds;
        }
        // ★ 收满之后**不能立刻退出**：最后一条的 PUBACK 还在异步写队列里，
        //   马上 io.stop() 会把它取消，发布端于是永远收不到确认、卡在积压里。
        //   所以要求「收满 且 再静默 1.5 秒」才收尾。
        if (o.expect > 0 && got >= o.expect && quiet_rounds >= 8) {
            done = true;
        }
    }

    // 接收端对账
    const std::uint64_t unique = broker.ledger().unique_count();
    const std::uint64_t missing = broker.ledger().missing_against_produced(o.expect);
    std::printf("\n接收端对账:\n");
    std::printf("  唯一接收（消息）: %llu\n", static_cast<unsigned long long>(unique));
    std::printf("  点位合计        : %llu\n",
                static_cast<unsigned long long>(broker.ledger().points_received()));
    std::printf("  生产端最高 seq  : %llu\n", static_cast<unsigned long long>(o.expect));
    std::printf("  ★ 丢失（消息）   : %llu\n", static_cast<unsigned long long>(missing));
    std::printf("  重复投递        : %llu\n",
                static_cast<unsigned long long>(broker.duplicate_publishes()));
    std::printf("  非法载荷        : %llu\n", static_cast<unsigned long long>(broker.bad_payloads()));
    std::printf("  连接次数        : %llu\n", static_cast<unsigned long long>(broker.connects()));

    if (o.expect > 0) {
        check(missing == 0, "★ 端到端丢失必须为 0");
        check(broker.duplicate_publishes() == 0, "重复投递必须为 0");
        check(broker.bad_payloads() == 0, "载荷必须全部可解析");
        if (o.expect_points > 0) {
            check(broker.ledger().points_received() == o.expect_points,
                  "★ 点位合计必须与生产端一致");
        }
        std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    }

    work_guard.reset();
    io.stop();
    worker.join();
    return g_failed == 0 ? 0 : 1;
}

// ── 发布角色 ────────────────────────────────────────────────────────────────
int run_pub(const Options& o) {
    SimClock clock;
    gw::MqttTransport tx(make_cfg(o, "gw-proxy-pub"));
    gw::ProxyConfig cfg;
    cfg.store_capacity = 100000;
    cfg.store_ttl_ms = 24ULL * 3600 * 1000;
    gw::RecordStore store(cfg.store_capacity, cfg.store_ttl_ms);
    gw::DataProxy proxy(store, tx, clock, cfg);

    // 角色默认值
    const std::uint64_t cycles = o.count != 0 ? o.count : 200;
    const std::uint64_t pts_per_cycle = o.points_per_cycle != 0 ? o.points_per_cycle : 1;

    const std::uint64_t outage_from = cycles / 4 * o.period_ms;
    const std::uint64_t outage_to = outage_from + o.outage_sec * 1000;
    bool outage_done = (o.outage_sec == 0);
    bool in_outage = false;

    // 断网推进必须在**主循环与排空循环里都调用**：
    // 否则一旦恢复时刻落在主循环之外（本例就是这样：窗口到 2500ms，主循环只到 1990ms），
    // 链路会永远停在 down，积压永远排不空。
    auto drive_outage = [&](std::uint64_t t) {
        if (o.outage_sec == 0) {
            return;
        }
        if (!in_outage && t >= outage_from) {
            // 对真 broker 时不能靠"杀掉 broker"制造断网（broker 不在我们手里），
            // 所以显式告知传输层链路不可用 —— 等价于网卡 down / 路由不可达事件。
            tx.set_link_available(false);
            in_outage = true;
        }
        if (in_outage && !outage_done && t >= outage_to) {
            tx.set_link_available(true);
            outage_done = true;   // 恢复（下一次重试会自动重连）
        }
    };

    std::printf("发布侧: %s:%u topic=%s\n", o.host.c_str(), o.port, o.topic.c_str());
    std::printf("  采样: %llu 个周期 × %llu 点位/周期 = %llu 点位（%llu 条消息），周期 %llu ms\n",
                static_cast<unsigned long long>(cycles),
                static_cast<unsigned long long>(pts_per_cycle),
                static_cast<unsigned long long>(cycles * pts_per_cycle),
                static_cast<unsigned long long>(cycles),
                static_cast<unsigned long long>(o.period_ms));
    if (o.outage_sec > 0) {
        std::printf("  断网: 第 %llu ms 起持续 %llu 秒（主动断连接，等价于链路 down）\n",
                    static_cast<unsigned long long>(outage_from),
                    static_cast<unsigned long long>(o.outage_sec));
    }

    gw::LinkState prev = proxy.state();
    for (std::uint64_t i = 0; i < cycles; ++i) {
        const std::uint64_t t = i * o.period_ms;
        drive_outage(t);
        // 一个采样周期：把本周期所有点位加进来，末端封成一条消息
        for (std::uint64_t p = 0; p < pts_per_cycle; ++p) {
            proxy.add_point(static_cast<std::uint16_t>(p),
                            static_cast<std::uint16_t>((i * 7 + p) & 0xFFFF));
        }
        proxy.end_cycle();
        clock.advance(o.period_ms);
        proxy.tick();

        if (proxy.state() != prev) {
            std::printf("  t=%6llu ms  %s（积压 %zu 条消息）\n",
                        static_cast<unsigned long long>(t),
                        proxy.state() == gw::LinkState::Live       ? "Live"
                        : proxy.state() == gw::LinkState::Backfill ? "Backfill"
                                                                  : "CatchingUp",
                        store.size());
            prev = proxy.state();
        }
    }

    // 收尾：给足机会把积压排空（排空期间也要继续推进断网状态），并设墙上时间上限
    const auto wall_start = std::chrono::steady_clock::now();
    std::uint64_t tail_t = cycles * o.period_ms;
    for (int i = 0; i < 4000 && !store.empty(); ++i) {
        tail_t += 50;
        drive_outage(tail_t);
        clock.advance(50);
        proxy.tick();
        if (i % 20 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (std::chrono::steady_clock::now() - wall_start > std::chrono::seconds(30)) {
            std::printf("  [FAIL] 排空超时（30s 墙上时间），剩余 %zu 条；最后错误: %s\n",
                        store.size(), tx.last_error().c_str());
            break;
        }
    }

    const std::uint64_t highest = proxy.produced_highest_seq();
    std::printf("\n发布侧统计:\n");
    std::printf("  生产消息   : %llu 条（最高 seq %llu）\n",
                static_cast<unsigned long long>(proxy.stats().produced),
                static_cast<unsigned long long>(highest));
    std::printf("  生产点位   : %llu 个（%.1f 点位/消息）\n",
                static_cast<unsigned long long>(proxy.stats().points),
                proxy.stats().produced == 0
                    ? 0.0
                    : static_cast<double>(proxy.stats().points) /
                          static_cast<double>(proxy.stats().produced));
    std::printf("  成功发送   : %llu（重试 %llu）\n",
                static_cast<unsigned long long>(proxy.stats().sent),
                static_cast<unsigned long long>(proxy.stats().retries));
    std::printf("  积压峰值   : %llu 条消息 / 重连 %llu 次\n",
                static_cast<unsigned long long>(proxy.stats().backlog_peak),
                static_cast<unsigned long long>(proxy.stats().reconnects));
    std::printf("  未排空     : %zu 条消息\n", store.size());
    std::printf("  供接收端对账: --expect %llu --expect-points %llu\n",
                static_cast<unsigned long long>(highest),
                static_cast<unsigned long long>(proxy.stats().points));

    check(store.size() == 0, "结束时积压应排空");
    check(proxy.state() == gw::LinkState::Live, "结束时状态应为 Live");
    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// ── 订阅角色（对着真 broker 时充当接收端）───────────────────────────────────
//
// Async API 没有 receive() 式收包循环 —— 消息经 messageArrived 回调投递，
// 用互斥锁保护地攒进队列，主循环攒一批喂一次对账器。
// （旧注释里「回调与 receive 混用会重复处理」是同步客户端的坑，纯回调通道不存在该问题。）
namespace {

struct SubState {
    std::mutex mtx;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> received;  // topic + payload
};

int on_message_arrived(void* context, char* topicName, int /*topicLen*/, MQTTAsync_message* m) {
    auto* st = static_cast<SubState*>(context);
    if (st != nullptr && m != nullptr) {
        std::pair<std::string, std::vector<std::uint8_t>> item;
        item.first = (topicName != nullptr) ? topicName : "";
        const auto* p = static_cast<const std::uint8_t*>(m->payload);
        item.second.assign(p, p + static_cast<std::size_t>(m->payloadlen));
        std::lock_guard<std::mutex> lk(st->mtx);
        st->received.push_back(std::move(item));
    }
    if (m != nullptr) {
        MQTTAsync_freeMessage(&m);
    }
    if (topicName != nullptr) {
        MQTTAsync_free(topicName);
    }
    return 1;
}

}  // namespace

int run_sub(const Options& o) {
    gw::Ledger ledger;
    SubState sub_state;

    const std::string address = "tcp://" + o.host + ":" + std::to_string(o.port);
    MQTTAsync sub = nullptr;
    if (MQTTAsync_create(&sub, address.c_str(), "gw-verify-sub", MQTTCLIENT_PERSISTENCE_NONE,
                         nullptr) != MQTTASYNC_SUCCESS) {
        std::printf("创建订阅客户端失败\n");
        return 1;
    }
    MQTTAsync_setCallbacks(sub, &sub_state, nullptr, on_message_arrived, nullptr);
    MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
    opts.keepAliveInterval = 20;
    opts.cleansession = 1;
    if (MQTTAsync_connect(sub, &opts) != MQTTASYNC_SUCCESS) {
        std::printf("连接 %s 失败\n", address.c_str());
        return 1;
    }
    // 异步连接：等 CONNACK（本地 broker 亚毫秒级，这里给足墙上时间上限）
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!MQTTAsync_isConnected(sub)) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::printf("连接 %s 超时\n", address.c_str());
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    MQTTAsync_responseOptions sresp = MQTTAsync_responseOptions_initializer;
    if (MQTTAsync_subscribe(sub, o.topic.c_str(), 0, &sresp) != MQTTASYNC_SUCCESS ||
        MQTTAsync_waitForCompletion(sub, sresp.token, 5000) != MQTTASYNC_SUCCESS) {
        std::printf("订阅 %s 失败\n", o.topic.c_str());
        return 1;
    }
    std::printf("订阅侧: %s topic=%s\n", address.c_str(), o.topic.c_str());
    if (o.expect > 0) {
        std::printf("  期望生产端最高 seq = %llu（收满后自动对账）\n",
                    static_cast<unsigned long long>(o.expect));
    }

    std::uint64_t last = 0;
    for (;;) {
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> batch;
        {
            std::lock_guard<std::mutex> lk(sub_state.mtx);
            batch.swap(sub_state.received);
        }
        for (auto& item : batch) {
            (void)item.first;   // --sub 模式只对账本项目的 RecordCodec 载荷
            gw::Record rec;
            if (gw::RecordCodec::decode(item.second.data(), item.second.size(), rec)) {
                ledger.accept(rec);
            }
        }
        const std::uint64_t got = ledger.unique_count();
        if (got != last) {
            last = got;
            std::printf("  已收到 %llu 条\n", static_cast<unsigned long long>(got));
            std::fflush(stdout);
        }
        if (o.expect > 0 && got >= o.expect) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const std::uint64_t missing = ledger.missing_against_produced(o.expect);
    std::printf("\n接收端对账:\n");
    std::printf("  唯一接收（消息）: %llu / 生产端最高 %llu\n",
                static_cast<unsigned long long>(ledger.unique_count()),
                static_cast<unsigned long long>(o.expect));
    std::printf("  点位合计        : %llu\n",
                static_cast<unsigned long long>(ledger.points_received()));
    std::printf("  ★ 丢失（消息）   : %llu\n", static_cast<unsigned long long>(missing));
    std::printf("  重复投递        : %llu\n", static_cast<unsigned long long>(ledger.duplicate_count()));
    check(missing == 0, "★ 端到端丢失必须为 0");
    check(ledger.duplicate_count() == 0, "重复投递必须为 0");
    if (o.expect_points > 0) {
        check(ledger.points_received() == o.expect_points, "★ 点位合计必须与生产端一致");
    }

    MQTTAsync_responseOptions uresp = MQTTAsync_responseOptions_initializer;
    MQTTAsync_unsubscribe(sub, o.topic.c_str(), &uresp);
    MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
    dopts.timeout = 1000;
    MQTTAsync_disconnect(sub, &dopts);
    MQTTAsync_destroy(&sub);
    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// ── 吞吐对比：批量打包 vs 一点位一消息（同一台机器、同一个 broker）────────────
//
// 存在的理由：实测 Paho 同步 API 单条发布 ~116ms。这个 bench 把「打包前后差多少」
// 用同一批点位数直接量出来，作为方案 §7 性能口径的依据。
int run_bench(const Options& o) {
    // 默认 200 点位：模式 A 要发 200 条（≈23 秒），够看出差距又不至于跑太久。
    // 需要更可信的曲线时用 --count 显式指定更大的总点位数。
    const std::uint64_t total_points = o.count != 0 ? o.count : 200;
    const std::uint64_t batch = o.points_per_cycle != 0 ? o.points_per_cycle : 50;

    std::printf("=========== 批量打包 vs 一点位一消息（真实 MQTT）===========\n");
    asio::io_context io;
    gw::testing::MiniMqttBroker broker(io);
    if (!broker.start(0)) {
        std::printf("broker 启动失败\n");
        return 1;
    }
    auto work_guard = asio::make_work_guard(io);
    std::thread worker([&io] { io.run(); });
    const std::uint16_t port = broker.port();
    std::printf("  broker: 127.0.0.1:%u（真 TCP + 真 MQTT 帧）\n", port);
    std::printf("  总点位数: %llu；批量大小: %llu\n\n", static_cast<unsigned long long>(total_points),
                static_cast<unsigned long long>(batch));

    struct Result {
        std::uint64_t messages = 0;
        std::uint64_t points = 0;
        double ms = 0;
        double points_per_s = 0;
        double msg_per_s = 0;
    };

    auto run_one = [&](std::uint64_t pts_per_msg) -> Result {
        SimClock clock;
        gw::MqttConfig cfg = make_cfg(o, "gw-bench");
        cfg.port = port;
        gw::MqttTransport tx(cfg);
        gw::ProxyConfig pc;
        gw::RecordStore store(500000, 24ULL * 3600 * 1000);
        gw::DataProxy proxy(store, tx, clock, pc);

        const std::uint64_t cycles = (total_points + pts_per_msg - 1) / pts_per_msg;
        // 预热：建连与 Paho 后台线程的一次性启动成本不计入吞吐 —— 否则小样本
        // 下偶发的 ~110ms 建连抖动会翻转 A/B 对比（实测踩过，ctest 的 bench
        // 用例曾因此间歇性失败）。
        if (!tx.connect()) {
            check(false, "bench 预热连接失败: " + tx.last_error());
            return Result{};
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (std::uint64_t c = 0; c < cycles; ++c) {
            for (std::uint64_t p = 0; p < pts_per_msg; ++p) {
                proxy.add_point(static_cast<std::uint16_t>(p), static_cast<std::uint16_t>(p));
            }
            proxy.end_cycle();
            clock.advance(1);
            proxy.tick();
        }
        for (int i = 0; i < 2000000 && !store.empty(); ++i) {   // 排空
            clock.advance(1);
            proxy.tick();
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        Result r;
        r.messages = proxy.stats().produced;
        r.points = proxy.stats().points;
        r.ms = ms;
        r.points_per_s = ms > 0 ? static_cast<double>(r.points) * 1000.0 / ms : 0;
        r.msg_per_s = ms > 0 ? static_cast<double>(r.messages) * 1000.0 / ms : 0;
        check(store.size() == 0, "bench 结束时积压应排空");
        return r;
    };

    std::printf("  模式 A：一点位一消息 …\n");
    const Result a = run_one(1);
    std::printf("  模式 B：%llu 点位/消息 …\n", static_cast<unsigned long long>(batch));
    const Result b = run_one(batch);

    std::printf("\n  结果:\n");
    std::printf("    模式 A（1 点位/消息）  : %llu 点位 / %llu 条消息，耗时 %.0f ms"
                " ⇒ %.1f 点位/秒，%.1f 消息/秒\n",
                static_cast<unsigned long long>(a.points),
                static_cast<unsigned long long>(a.messages), a.ms, a.points_per_s, a.msg_per_s);
    std::printf("    模式 B（%llu 点位/消息）: %llu 点位 / %llu 条消息，耗时 %.0f ms"
                " ⇒ %.1f 点位/秒，%.1f 消息/秒\n",
                static_cast<unsigned long long>(batch),
                static_cast<unsigned long long>(b.points),
                static_cast<unsigned long long>(b.messages), b.ms, b.points_per_s, b.msg_per_s);
    if (a.points_per_s > 0) {
        std::printf("    ★ 点位吞吐提升        : %.1f×（消息数减少 %.1f×）\n",
                    b.points_per_s / a.points_per_s,
                    a.messages > 0 ? static_cast<double>(a.messages) /
                                         static_cast<double>(b.messages)
                                   : 0.0);
    }
    std::printf("\n  说明：传输层已改 Paho Async + 事件驱动等 PUBACK（R9 改造），"
                "同步时代的 ~116ms/条 地板已消除。\n        打包仍有价值：消息数更少 ⇒ 队列/网络开销更小，"
                "吞吐更高（见上方倍数）。\n");

    check(b.points == a.points, "两种模式应发送相同点位数（可比性前提）");
    check(b.messages < a.messages, "批量模式的消息数应更少");
    check(b.points_per_s > a.points_per_s, "批量模式的点位吞吐应更高");

    work_guard.reset();
    io.stop();
    worker.join();
    broker.stop();
    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// ── 原始订阅：用来验证**别人的** MQTT 输出（比如 Neuron 自己的载荷格式）──────
//
// 为什么单独一个模式：--sub 期望的是本项目的 RecordCodec 载荷；
// 要验证 Neuron 这类第三方网关的发布，只需把 topic + 原始载荷打出来看。
// 同样走 Async 回调通道（SubState 攒队列，主循环打印）。
int run_rawsub(const Options& o) {
    SubState raw_state;

    const std::string address = "tcp://" + o.host + ":" + std::to_string(o.port);
    MQTTAsync sub = nullptr;
    if (MQTTAsync_create(&sub, address.c_str(), "gw-rawsub", MQTTCLIENT_PERSISTENCE_NONE,
                         nullptr) != MQTTASYNC_SUCCESS) {
        std::printf("创建订阅客户端失败\n");
        return 1;
    }
    MQTTAsync_setCallbacks(sub, &raw_state, nullptr, on_message_arrived, nullptr);
    MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
    opts.keepAliveInterval = 20;
    opts.cleansession = 1;
    if (MQTTAsync_connect(sub, &opts) != MQTTASYNC_SUCCESS) {
        std::printf("连接 %s 失败\n", address.c_str());
        return 1;
    }
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!MQTTAsync_isConnected(sub)) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::printf("连接 %s 超时\n", address.c_str());
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    MQTTAsync_responseOptions sresp = MQTTAsync_responseOptions_initializer;
    if (MQTTAsync_subscribe(sub, o.topic.c_str(), 0, &sresp) != MQTTASYNC_SUCCESS ||
        MQTTAsync_waitForCompletion(sub, sresp.token, 5000) != MQTTASYNC_SUCCESS) {
        std::printf("订阅 %s 失败\n", o.topic.c_str());
        return 1;
    }
    std::printf("原始订阅: %s topic=%s（最多等 %llu 秒，收到 %llu 条后退出）\n", address.c_str(),
                o.topic.c_str(), static_cast<unsigned long long>(o.outage_sec == 0 ? 15 : o.outage_sec),
                static_cast<unsigned long long>(o.count));

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(o.outage_sec == 0 ? 15 : o.outage_sec);
    std::uint64_t got = 0;
    while (got < o.count && std::chrono::steady_clock::now() < deadline) {
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> batch;
        {
            std::lock_guard<std::mutex> lk(raw_state.mtx);
            batch.swap(raw_state.received);
        }
        if (batch.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        for (auto& item : batch) {
            ++got;
            std::string payload(item.second.begin(), item.second.end());
            std::printf("  #%llu topic=%s len=%zu payload=%.300s\n",
                        static_cast<unsigned long long>(got),
                        item.first.empty() ? "(null)" : item.first.c_str(), item.second.size(),
                        payload.c_str());
            std::fflush(stdout);
        }
    }
    check(got >= 1, "★ 应至少收到 1 条北向报文");
    MQTTAsync_responseOptions uresp = MQTTAsync_responseOptions_initializer;
    MQTTAsync_unsubscribe(sub, o.topic.c_str(), &uresp);
    MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
    dopts.timeout = 1000;
    MQTTAsync_disconnect(sub, &dopts);
    MQTTAsync_destroy(&sub);
    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// ── 自检：进程内 broker + 发布 + 脚本化断网 + 自动对账 ──────────────────────
int run_selftest(const Options& o) {
    std::printf("=========== MQTT 端到端自检（断网续传）===========\n");

    // ── 先验编码器：批量格式的往返与边界 ─────────────────────────────────────
    {
        gw::Record r;
        r.seq = 42;
        r.ts_ms = 123456789;
        for (std::uint16_t i = 0; i < 7; ++i) {
            r.points.push_back(gw::Point{i, static_cast<std::uint16_t>(i * 11 + 1)});
        }
        const std::vector<std::uint8_t> wire = gw::RecordCodec::encode(r);
        check(wire.size() == gw::RecordCodec::kHeaderBytes + 7 * gw::RecordCodec::kPointBytes,
              "编码长度应为 22+4N");

        gw::Record back;
        check(gw::RecordCodec::decode(wire.data(), wire.size(), back), "批量载荷应能解码");
        check(back.seq == r.seq && back.ts_ms == r.ts_ms, "解码后 seq/ts 应一致");
        check(back.points.size() == 7, "解码后点位数应一致");
        bool same = (back.points.size() == r.points.size());
        for (std::size_t i = 0; same && i < r.points.size(); ++i) {
            same = (back.points[i].addr == r.points[i].addr) &&
                   (back.points[i].value == r.points[i].value);
        }
        check(same, "解码后点位内容应逐项一致");

        // 负例：长度不符 / 点位数与长度不符 / 空批，都必须被拒
        check(!gw::RecordCodec::decode(wire.data(), wire.size() - 1, back), "长度不符应被拒绝");
        std::vector<std::uint8_t> bad_count = wire;
        bad_count[20] = 0x00;
        bad_count[21] = 0x63;   // 声明 99 个点位，实际 7 个
        check(!gw::RecordCodec::decode(bad_count.data(), bad_count.size(), back),
              "点位数与长度不符应被拒绝");
        gw::Record empty;
        check(gw::RecordCodec::encode(empty).empty(), "空批不应被编码");
        std::printf("  编码器：批量格式往返与边界检查完成\n");
    }

    asio::io_context io;
    gw::testing::MiniMqttBroker broker(io);
    if (!broker.start(0)) {
        std::printf("broker 启动失败\n");
        return 1;
    }
    std::thread worker([&io] { io.run(); });
    const std::uint16_t port = broker.port();

    // ★ asio 陷阱一：io_context 没有未完成工作时 io.run() 会立刻返回。
    //   broker.stop() 之后监听与连接都没了 ⇒ io 线程会直接退出，
    //   之后 post 进去的东西永远不执行，主线程死等。
    //   解法：用 work guard 保住 io 线程，直到我们主动释放。
    auto work_guard = asio::make_work_guard(io);

    // ★ asio 陷阱二：asio 对象不是线程安全的。broker 的 acceptor/session 归 io 线程所有，
    //   从主线程直接调 stop()/start() 会在 async_accept 在途时跨线程改状态。
    //   实测表现：「broker 重启后其实没监听上，客户端 connect 一直失败」。
    auto run_on_io = [&io](const std::function<void()>& fn) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        asio::post(io, [fn, done] {
            fn();
            done->store(true);
        });
        while (!done->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    std::printf("  最小 MQTT broker: 127.0.0.1:%u（内核分配，真 TCP + 真 MQTT 帧）\n", port);

    // 看门狗，避免 CI 挂死
    std::thread([&] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        std::printf("    [FAIL] 自检超时（120s）\n");
        std::_Exit(3);
    }).detach();

    Options po = o;
    po.host = "127.0.0.1";
    po.port = port;
    po.period_ms = 20;
    // 默认 40 个采样周期 × 40 点位/周期 = 1600 点位，只需 **40 条消息** ⇒ 约 5 秒。
    // 对比：同样 1600 点位若一点位一消息，按实测 116ms/条要 3 分钟以上 —— 这就是打包的意义。
    po.count = o.count != 0 ? o.count : 40;
    po.points_per_cycle = o.points_per_cycle != 0 ? o.points_per_cycle : 40;
    po.outage_sec = 3;

    SimClock clock;
    gw::MqttConfig selftest_cfg = make_cfg(po, "gw-proxy-selftest");
    // 断网时第一次发送要靠这个超时发现「连接其实已经死了」（TCP 未必立刻报错）
    selftest_cfg.ack_timeout_ms = 600;
    gw::MqttTransport tx(selftest_cfg);
    gw::ProxyConfig cfg;
    cfg.store_capacity = 100000;
    cfg.store_ttl_ms = 24ULL * 3600 * 1000;
    gw::RecordStore store(cfg.store_capacity, cfg.store_ttl_ms);
    gw::DataProxy proxy(store, tx, clock, cfg);

    // 断网窗口：第 1/4 处开始。★ 窗口长度必须夹在总时长之内 ——
    // 否则「恢复」那一刻永远不会被触发，链路会一直停在 down（这个坑踩过两次）。
    const std::uint64_t total_logical = po.count * po.period_ms;
    const std::uint64_t outage_from = total_logical / 4;
    const std::uint64_t outage_ms =
        std::min<std::uint64_t>(po.outage_sec * 1000, total_logical / 2);
    const std::uint64_t outage_to = outage_from + outage_ms;
    bool outage_started = false;
    bool outage_finished = false;

    // 断网推进必须在**主循环与排空循环里都调用**
    // 注意用两个独立标志：只用一个"当前是否 down"会在恢复后被反复重启
    //（实测症状：日志里每 50ms 打印一次"关掉/恢复"）。
    auto drive_outage = [&](std::uint64_t t) {
        if (!outage_started && t >= outage_from) {
            run_on_io([&] { broker.stop(); });
            outage_started = true;
            std::printf("    t=%6llu ms  —— 关掉 broker（模拟断网）——\n",
                        static_cast<unsigned long long>(t));
        }
        if (outage_started && !outage_finished && t >= outage_to) {
            bool ok_start = false;
            run_on_io([&] { ok_start = broker.start(port); });
            outage_finished = true;
            std::printf("    t=%6llu ms  —— broker 恢复（同端口 %u）%s——\n",
                        static_cast<unsigned long long>(t), port, ok_start ? "" : "失败！");
        }
    };

    gw::LinkState prev = proxy.state();
    std::printf("\n  时间轴（仅在状态变化时打印）:\n");
    std::printf("    t=%6llu ms  %-11s 产出 %-5llu 积压 %zu\n", 0ULL, "Live", 0ULL,
                store.size());

    for (std::uint64_t i = 0; i < po.count; ++i) {
        const std::uint64_t t = i * po.period_ms;
        drive_outage(t);
        for (std::uint64_t p = 0; p < po.points_per_cycle; ++p) {
            proxy.add_point(static_cast<std::uint16_t>(p),
                            static_cast<std::uint16_t>((i * 7 + p) & 0xFFFF));
        }
        proxy.end_cycle();
        clock.advance(po.period_ms);
        proxy.tick();
        // 注入时钟跑得比墙钟快，让真实网络/重连有机会推进
        if ((i % 25) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (proxy.state() != prev) {
            std::printf("    t=%6llu ms  %-11s 产出 %-5llu 积压 %zu\n",
                        static_cast<unsigned long long>(t),
                        proxy.state() == gw::LinkState::Live       ? "Live"
                        : proxy.state() == gw::LinkState::Backfill ? "Backfill"
                                                                   : "CatchingUp",
                        static_cast<unsigned long long>(proxy.stats().produced), store.size());
            prev = proxy.state();
        }
    }

    // 收尾：把积压排空。注入时钟让"逻辑时间"能瞬间跑完，但**网络是真实时间**，
    // 所以这里必须有一个墙上时间上限 —— 否则一旦重连逻辑有问题就会干等到看门狗。
    const auto wall_start = std::chrono::steady_clock::now();
    std::uint64_t tail_t = total_logical;
    for (int i = 0; i < 4000 && !store.empty(); ++i) {
        tail_t += 50;
        drive_outage(tail_t);   // ★ 排空期间也要继续推进断网状态
        clock.advance(50);
        proxy.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() - wall_start > std::chrono::seconds(30)) {
            std::printf("    [FAIL] 排空超时（30s 墙上时间），剩余积压 %zu 条；"
                        "最后错误: %s\n",
                        store.size(), tx.last_error().c_str());
            break;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 等最后一批 PUBACK/PUBLISH 落地

    const std::uint64_t produced = proxy.stats().produced;
    const std::uint64_t produced_points = proxy.stats().points;
    const std::uint64_t highest = proxy.produced_highest_seq();
    const std::uint64_t got = broker.ledger().unique_count();
    const std::uint64_t got_points = broker.ledger().points_received();
    const std::uint64_t missing = broker.ledger().missing_against_produced(highest);
    const std::uint64_t dup = broker.duplicate_publishes();

    std::printf("\n  结果（接收端对账 = broker 侧统计）:\n");
    std::printf("    生产           : %llu 条消息 / %llu 点位（最高 seq %llu）\n",
                static_cast<unsigned long long>(produced),
                static_cast<unsigned long long>(produced_points),
                static_cast<unsigned long long>(highest));
    std::printf("    broker 唯一接收: %llu 条消息 / %llu 点位\n",
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(got_points));
    std::printf("    ★ 端到端丢失    : %llu 条消息\n", static_cast<unsigned long long>(missing));
    std::printf("    重复投递        : %llu 条\n", static_cast<unsigned long long>(dup));
    std::printf("    非法载荷        : %llu 条\n",
                static_cast<unsigned long long>(broker.bad_payloads()));
    std::printf("    发布侧成功发送  : %llu（重试 %llu）\n",
                static_cast<unsigned long long>(proxy.stats().sent),
                static_cast<unsigned long long>(proxy.stats().retries));
    std::printf("    积压峰值        : %llu 条消息 / 重连 %llu 次\n",
                static_cast<unsigned long long>(proxy.stats().backlog_peak),
                static_cast<unsigned long long>(proxy.stats().reconnects));
    std::printf("    broker 连接次数 : %llu\n", static_cast<unsigned long long>(broker.connects()));

    std::printf("\n  判据:\n");
    check(produced == po.count, "消息数应等于采样周期数");
    check(produced_points == po.count * po.points_per_cycle, "点位数应等于周期数×每周期点位数");
    check(missing == 0, "★ 端到端丢失必须为 0");
    check(dup == 0, "重复投递必须为 0");
    check(got_points == produced_points, "★ 接收端点位数必须与生产端一致");
    check(broker.bad_payloads() == 0, "载荷必须全部可解码");
    check(store.size() == 0, "结束时积压应排空");
    check(proxy.state() == gw::LinkState::Live, "结束时状态应为 Live");
    check(proxy.stats().reconnects >= 1, "断网恢复后应至少发生一次重连");
    check(proxy.stats().backlog_peak > 0, "断网期间应产生积压");

    // io 线程退出后再动 broker —— 此时单线程访问才安全
    work_guard.reset();
    io.stop();
    worker.join();
    broker.stop();
    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        return 2;
    }
    if (opt.role.empty() || opt.role == "selftest") {
        return run_selftest(opt);
    }
    if (opt.role == "broker") {
        return run_broker(opt);
    }
    if (opt.role == "bench") {
        return run_bench(opt);
    }
    if (opt.role == "pub") {
        return run_pub(opt);
    }
    if (opt.role == "sub") {
        return run_sub(opt);
    }
    if (opt.role == "rawsub") {
        return run_rawsub(opt);
    }
    usage();
    return 2;
}
