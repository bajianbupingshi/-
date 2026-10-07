#include "gw/alert_publisher.h"

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

extern "C" {
#include "MQTTAsync.h"
}

namespace gw {
namespace {

using json = nlohmann::json;

// 单行 JSON，键名字典序（nlohmann 的 map 排序）；detail 由引擎确定性格式化，
// nlohmann dump 负责转义 —— 载荷形状跨版本稳定，订阅端可依赖
std::string alert_to_json(const Alert& a) {
    json j;
    j["rule"] = a.rule_name;
    j["kind"] = a.kind;
    j["addr"] = a.addr;
    j["value"] = a.value;
    j["ts_ms"] = a.ts_ms;
    j["detail"] = a.detail;
    return j.dump();
}

bool connect_with_wait(MQTTAsync h, const AlertPublisherConfig& cfg, std::string& err) {
    MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
    opts.keepAliveInterval = cfg.keep_alive_s;
    opts.cleansession = 1;
    opts.connectTimeout = 5;
    const int rc = MQTTAsync_connect(h, &opts);
    if (rc != MQTTASYNC_SUCCESS) {
        err = "connect failed rc=" + std::to_string(rc);
        return false;
    }
    // 异步连接：等 CONNACK（isConnected 翻真）。告警低频，2ms 轮询的墙上成本可忽略
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(cfg.ack_timeout_ms);
    while (!MQTTAsync_isConnected(h)) {
        if (std::chrono::steady_clock::now() > deadline) {
            err = "connect 超时（CONNACK 未在时限内到达）";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

}  // namespace

struct AlertPublisher::Impl {
    explicit Impl(const AlertPublisherConfig& c) : cfg(c) {}

    ~Impl() {
        if (handle != nullptr) {
            MQTTAsync_destroy(reinterpret_cast<MQTTAsync*>(&handle));
        }
    }

    bool ensure_connected(std::string& err) {
        if (handle != nullptr && MQTTAsync_isConnected(handle) != 0) {
            return true;
        }
        if (handle == nullptr) {
            const std::string address = "tcp://" + cfg.host + ":" + std::to_string(cfg.port);
            if (MQTTAsync_create(&handle, address.c_str(), cfg.client_id.c_str(),
                                 MQTTCLIENT_PERSISTENCE_NONE, nullptr) != MQTTASYNC_SUCCESS) {
                err = "MQTTAsync_create failed";
                return false;
            }
        }
        if (!connect_with_wait(handle, cfg, err)) {
            // 连接不可信：拆掉让下次从零重建
            MQTTAsync_destroy(reinterpret_cast<MQTTAsync*>(&handle));
            handle = nullptr;
            return false;
        }
        return true;
    }

    AlertPublisherConfig cfg;
    MQTTAsync handle = nullptr;
};

AlertPublisher::AlertPublisher(AlertPublisherConfig cfg)
    : impl_(std::make_unique<Impl>(cfg)), cfg_(std::move(cfg)) {}

AlertPublisher::~AlertPublisher() = default;

void AlertPublisher::disconnect() {
    if (impl_->handle != nullptr) {
        MQTTAsync_disconnectOptions dopts = MQTTAsync_disconnectOptions_initializer;
        dopts.timeout = 0;
        MQTTAsync_disconnect(impl_->handle, &dopts);
        MQTTAsync_destroy(reinterpret_cast<MQTTAsync*>(&impl_->handle));
    }
}

bool AlertPublisher::connect() {
    return impl_->ensure_connected(last_error_);
}

bool AlertPublisher::publish(const Alert& alert) {
    if (!impl_->ensure_connected(last_error_)) {
        ++failed_;
        return false;
    }

    const std::string payload = alert_to_json(alert);
    MQTTAsync_responseOptions resp = MQTTAsync_responseOptions_initializer;
    const int rc = MQTTAsync_send(impl_->handle, cfg_.topic.c_str(),
                                  static_cast<int>(payload.size()), payload.c_str(), cfg_.qos,
                                  0, &resp);
    if (rc != MQTTASYNC_SUCCESS) {
        last_error_ = "publish failed rc=" + std::to_string(rc);
        ++failed_;
        return false;
    }
    // QoS1 等 PUBACK。告警低频（冷却后秒级一条），waitForCompletion 的 100ms
    // 轮询粒度在这里无关紧要 —— 不为此复用传输层的事件驱动完成槽。
    if (MQTTAsync_waitForCompletion(impl_->handle, resp.token,
                                    static_cast<unsigned long>(cfg_.ack_timeout_ms)) !=
        MQTTASYNC_SUCCESS) {
        last_error_ = "waitForCompletion failed（PUBACK 超时）";
        ++failed_;
        return false;
    }
    ++published_;
    return true;
}

}  // namespace gw
