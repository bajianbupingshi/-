// 告警发布链路端到端（W4-1）：
//   采集线程 produce → 管道线程(规则引擎求值) → AlertPublisher ──真 TCP/MQTT──► mini broker
//   ▲ 订阅端（MQTTAsync 回调）独立收 JSON 告警，验证载荷形状与内容。
// 需要规则引擎 + MQTT 同时开启（TARGET gw_alerts）。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest_shim.h"
#include "gw/alert_publisher.h"
#include "gw/proxy_service.h"
#include "gw/reliability.h"
#include "gw/rules.h"
#include "gw/testing/mini_mqtt_broker.h"

extern "C" {
#include "MQTTAsync.h"
}

namespace {

// 数据传输的空实现：mini broker 本身就是接收端（broker.ledger() 记数据），
// 告警走 AlertPublisher 的独立主题 —— 发送恒 Ok 即可
class NullTransport : public gw::ITransport {
public:
    gw::SendResult send(const gw::Record& /*rec*/) override {
        return gw::SendResult::Ok;
    }
};

// 轮询等待（真时钟）
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

// 独立订阅端：MQTTAsync 回调通道收原始报文（与 mqtt_e2e 的 --rawsub 同款模式）
class RawSubscriber {
public:
    ~RawSubscriber() { close(); }

    bool start(const std::string& host, std::uint16_t port, const std::string& topic) {
        const std::string address = "tcp://" + host + ":" + std::to_string(port);
        if (MQTTAsync_create(&h_, address.c_str(), "alert-sub", MQTTCLIENT_PERSISTENCE_NONE,
                             nullptr) != MQTTASYNC_SUCCESS) {
            return false;
        }
        MQTTAsync_setCallbacks(h_, this, nullptr, &RawSubscriber::on_message, nullptr);
        MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
        opts.keepAliveInterval = 20;
        opts.cleansession = 1;
        if (MQTTAsync_connect(h_, &opts) != MQTTASYNC_SUCCESS) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!MQTTAsync_isConnected(h_)) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        MQTTAsync_responseOptions sresp = MQTTAsync_responseOptions_initializer;
        if (MQTTAsync_subscribe(h_, topic.c_str(), 0, &sresp) != MQTTASYNC_SUCCESS ||
            MQTTAsync_waitForCompletion(h_, sresp.token, 5000) != MQTTASYNC_SUCCESS) {
            return false;
        }
        return true;
    }

    void close() {
        if (h_ != nullptr) {
            MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
            dopts.timeout = 0;
            MQTTAsync_disconnect(h_, &dopts);
            MQTTAsync_destroy(&h_);
            h_ = nullptr;
        }
    }

    std::vector<std::string> snapshot_and_clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::move(received_);
    }

private:
    static int on_message(void* context, char* topicName, int /*topicLen*/, MQTTAsync_message* m) {
        auto* self = static_cast<RawSubscriber*>(context);
        if (self != nullptr && m != nullptr) {
            const std::string payload(static_cast<const char*>(m->payload),
                                      static_cast<std::size_t>(m->payloadlen));
            std::lock_guard<std::mutex> lk(self->mtx_);
            self->received_.push_back(payload);
        }
        if (m != nullptr) {
            MQTTAsync_freeMessage(&m);
        }
        if (topicName != nullptr) {
            MQTTAsync_free(topicName);
        }
        return 1;
    }

    MQTTAsync h_ = nullptr;
    std::mutex mtx_;
    std::vector<std::string> received_;
};

// Paho 内部 trace（诊断用）：显示 CONNECT 命令是否出队、socket 是否建立
void paho_trace(MQTTASYNC_TRACE_LEVELS /*level*/, char* message) {
    std::printf("  [paho] %s\n", message);
}

TEST(AlertPublish, EndToEndOverRealMqtt) {
    MQTTAsync_setTraceCallback(&paho_trace);
    MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_MAXIMUM);
    // mini broker（真 TCP + 真 MQTT 帧）
    asio::io_context io;
    gw::testing::MiniMqttBroker broker(io);
    ASSERT_TRUE(broker.start(0));
    auto work_guard = asio::make_work_guard(io);
    std::thread io_thread([&io] { io.run(); });
    const std::uint16_t port = broker.port();

    const std::string alerts_topic = "neuron/gateway/alerts";

    // 规则 + 发布器。★ 生产顺序：发布器**先建连**（长驻服务随启动建连），
    // 订阅端后启 —— 见 alert_publisher.h 的 Paho 多客户端约束说明。
    gw::AlertPublisherConfig pcfg;
    pcfg.port = port;
    pcfg.topic = alerts_topic;
    pcfg.client_id = "gw-alerts-test";
    gw::AlertPublisher publisher(pcfg);
    EXPECT_TRUE(publisher.connect());

    RawSubscriber sub;
    ASSERT_TRUE(sub.start("127.0.0.1", port, alerts_topic));

    gw::RuleEngine engine = gw::RuleEngine::from_json(
        "{\"rules\":[{\"name\":\"t-high\",\"type\":\"threshold\",\"addr\":0,"
        "\"op\":\"gt\",\"value\":30000}]}");

    // 服务：管道线程 求值 → 发布（观测钩子同步调用，与真实接线一致）
    NullTransport tx;   // 数据由 broker 接收；告警走 AlertPublisher 的独立 JSON 主题
    gw::RecordStore store(100000, 3600000);
    gw::EdgeProxyService svc(store, tx, {}, 0, 1024, 5);
    std::atomic<int> publish_failures{0};
    svc.set_sample_observer([&](const std::vector<gw::Point>& batch, std::uint64_t now_ms) {
        for (const gw::Alert& a : engine.evaluate(batch, now_ms)) {
            if (!publisher.publish(a)) {
                ++publish_failures;
            }
        }
    });
    svc.start();

    // 阶梯爬升跨过阈值
    for (int i = 0; i <= 400; ++i) {
        svc.produce({gw::Point{0, static_cast<std::uint16_t>(i * 100)}});
    }

    // 订阅端收到 JSON 告警（载荷形状稳定：nlohmann 键名字典序）
    bool ok = wait_for(
        [&] {
            const std::vector<std::string> got = sub.snapshot_and_clear();
            for (const std::string& p : got) {
                // 垫片宏不支持 << 流式，失败细节走 printf（双框架兼容写法）
                if (p.find("\"rule\":\"t-high\"") == std::string::npos ||
                    p.find("\"kind\":\"threshold\"") == std::string::npos ||
                    p.find("\"addr\":0") == std::string::npos ||
                    p.find("\"ts_ms\":") == std::string::npos) {
                    std::printf("      [FAIL] 告警 JSON 形状不符: %s\n", p.c_str());
                }
                EXPECT_NE(p.find("\"rule\":\"t-high\""), std::string::npos);
                EXPECT_NE(p.find("\"kind\":\"threshold\""), std::string::npos);
                EXPECT_NE(p.find("\"addr\":0"), std::string::npos);
                EXPECT_NE(p.find("\"ts_ms\":"), std::string::npos);
                return true;   // 收到一条即成功（内容断言见上）
            }
            return false;
        },
        std::chrono::seconds(15));

    svc.stop();
    if (!ok) {
        std::printf("      publisher: published=%llu failed=%llu last_error=%s\n",
                    static_cast<unsigned long long>(publisher.published()),
                    static_cast<unsigned long long>(publisher.failed()),
                    publisher.last_error().c_str());
        std::printf("      broker: connects=%llu running=%d\n",
                    static_cast<unsigned long long>(broker.connects()),
                    broker.running() ? 1 : 0);
    }
    EXPECT_TRUE(ok);
    EXPECT_GE(publisher.published(), std::uint64_t{1});
    EXPECT_EQ(publish_failures.load(), 0);
    EXPECT_EQ(publisher.failed(), std::uint64_t{0});

    work_guard.reset();
    io.stop();
    io_thread.join();
}

}  // namespace
