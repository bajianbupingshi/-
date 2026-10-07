#ifndef GW_ALERT_PUBLISHER_H
#define GW_ALERT_PUBLISHER_H

#include <cstdint>
#include <memory>
#include <string>

#include "gw/rules.h"   // Alert（gw_rules 的 include 目录经链接传递）

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// 告警发布器（W4-1）：规则引擎的告警 → MQTT JSON 主题
//
// 定位（诚实声明）：告警是**尽力而为的通知**，不是数据 ——
//   · 不入续传队列：对账口径（丢失 0/重复 0）只覆盖数据记录，不覆盖告警；
//     链路断开时告警直接丢弃并计 failed（规则的冷却机制保证恢复后不会风暴）。
//   · 独立 MQTTAsync 客户端（独立 client_id / topic），与数据通道（MqttTransport）
//     互不影响：告警发不出去不拖累数据，数据拥堵不丢告警的独立可见性。
//   · QoS1 + 逐条等 PUBACK：告警低频（冷却后秒级），waitForCompletion 的
//     100ms 轮询粒度在此无关紧要 —— 不复用传输层的事件驱动完成槽。
//
// 载荷：单行 JSON（nlohmann 生成，键名字典序）：
//   {"addr":0,"detail":"value=41234 gt 40000","kind":"threshold",
//    "rule":"t-high","ts_ms":123456,"value":41234}
// 线程契约：与采集观测钩子同序 —— 只在管道线程调用（串行，无内部锁）。
// ─────────────────────────────────────────────────────────────────────────────

struct AlertPublisherConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 1883;
    std::string client_id = "gw-alerts";
    std::string topic = "neuron/gateway/alerts";
    int qos = 1;
    int keep_alive_s = 20;
    std::uint64_t ack_timeout_ms = 1500;   // 等 PUBACK；超时即拆连接重建
};

class AlertPublisher {
public:
    explicit AlertPublisher(AlertPublisherConfig cfg);
    ~AlertPublisher();
    AlertPublisher(const AlertPublisher&) = delete;
    AlertPublisher& operator=(const AlertPublisher&) = delete;

    // ★ 显式建连（幂等）。**必须在同进程任何其他 MQTTAsync 客户端建连之前调用**：
    //   Paho 1.3.13/1.3.14 实测——先建连的客户端正常，第二个客户端的 CONNACK
    //   永远到不了（broker 只见 1 次 TCP 连接；paho trace 显示新 socket 加入
    //   poll 集后从未就绪）。生产形态本就如此：发布器随服务启动先行建连，
    //   订阅端后启。未定位到 paho 源码级根因（wip），约束先行、注入档期。
    bool connect();

    // 发布一条告警。false = 连接失败或 PUBACK 超时（failed 计数 +1，详情在 last_error）
    bool publish(const Alert& alert);

    void disconnect();   // 链路 down 事件可主动调用；下次 publish 惰性重连

    std::uint64_t published() const noexcept { return published_; }
    std::uint64_t failed() const noexcept { return failed_; }
    const std::string& last_error() const noexcept { return last_error_; }
    const AlertPublisherConfig& config() const noexcept { return cfg_; }

private:
    struct Impl;   // PIMPL：MQTTAsync.h 挡在 .cpp
    std::unique_ptr<Impl> impl_;
    AlertPublisherConfig cfg_;
    std::uint64_t published_ = 0;
    std::uint64_t failed_ = 0;
    std::string last_error_;
};

}  // namespace gw

#endif  // GW_ALERT_PUBLISHER_H
