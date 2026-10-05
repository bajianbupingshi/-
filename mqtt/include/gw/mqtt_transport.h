#ifndef GW_MQTT_TRANSPORT_H
#define GW_MQTT_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gw/reliability.h"

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// 真实 MQTT 传输（Paho MQTT C 的同步 API）
//
// 为什么用 Paho **C** 的同步 API，而不是计划里写的 Paho MQTT C++：
//   ITransport::send() 是**同步**语义（返回 Ok 表示对端已确认）。
//   Paho C 同步 API 的 MQTTClient_publishMessage + waitForCompletion 天然就是
//   「发出去 → 等到 PUBACK → 返回」，与 QoS1 一一对应；
//   而 C++ 包装的 Async 接口要额外引入事件循环 + 条件变量才能做成阻塞式，
//   反而多一层可能出错的机器，还多一个依赖。
//   ⇒ 已同步更新方案 §4 的技术栈描述。
//
// 头文件里**不出现任何 Paho 类型**（句柄用 void*），这样 gw_common 保持零依赖，
// Paho 被隔离在独立的 gw_mqtt target 里。
// ─────────────────────────────────────────────────────────────────────────────

// MQTT 载荷的确定性编码 —— **批量格式**（一条消息携带一个采样周期的 N 个点位）。
// 为什么自定义二进制而不是 JSON：紧凑、无浮点、跨语言逐位一致 ——
// 与方案 §7「跨语言对账」的要求一致。
//
//   偏移  长度  字段
//   0     2     MAGIC  'G''W'
//   2     1     VERSION = 2
//   3     1     保留（0）
//   4     8     seq      （大端）
//   12    8     ts_ms    （大端）
//   20    2     count    （点位个数，1..kMaxPoints）
//   22    4*N   N × { addr(2) + value(2) }（大端）
//   ------------------------------------------
//   合计 22 + 4N 字节；上限 22 + 4×1024 = 4118 字节
//
// 为什么必须批量：实测 Paho 同步 API 单条发布 116 ms（≈8.6 条/秒），
// 一点位一消息扛不住 500 点位 @100ms（=5000 点位/秒）；批量后只需 10 条消息/秒。
struct RecordCodec {
    static constexpr std::uint8_t kMagic0 = 0x47;  // 'G'
    static constexpr std::uint8_t kMagic1 = 0x57;  // 'W'
    static constexpr std::uint8_t kVersion = 2;
    static constexpr std::size_t kHeaderBytes = 22;
    static constexpr std::size_t kPointBytes = 4;
    static constexpr std::size_t kMaxPoints = 1024;
    static constexpr std::size_t kMaxBytes = kHeaderBytes + kMaxPoints * kPointBytes;

    static std::vector<std::uint8_t> encode(const Record& rec);
    // 解析失败（长度/魔数/版本/点位数不符）返回 false
    static bool decode(const std::uint8_t* data, std::size_t len, Record& out);
};

struct MqttConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 1883;
    std::string client_id = "gw-proxy";
    std::string topic = "neuron/gateway/data";
    std::string username;   // 空 = 不用认证
    std::string password;
    int qos = 1;
    int keep_alive_s = 20;
    std::uint64_t ack_timeout_ms = 2000;   // 等 PUBACK 的超时；超时即视为链路不可用
};

class MqttTransport : public ITransport {
public:
    explicit MqttTransport(MqttConfig cfg);
    MqttTransport(const MqttTransport&) = delete;
    MqttTransport& operator=(const MqttTransport&) = delete;
    ~MqttTransport() override;

    // 幂等：已连接时直接返回 true。失败时记录 last_error()。
    bool connect();
    void disconnect();
    bool is_connected();

    // 上层网络事件（网卡 down / 路由不可达 / NetworkManager 通知）可直接告知传输层。
    // 置 false 时会顺带断开连接（链路 down ⇒ 连接必然失效）；
    // 置回 true 后，下一次 send() 会惰性重连 —— 重试节奏仍由状态机控制。
    void set_link_available(bool up);
    bool link_available() const noexcept { return link_available_; }

    // QoS1：只有收到 PUBACK 才返回 Ok；任何失败都返回 Retry（由状态机决定何时重试）
    SendResult send(const Record& rec) override;

    const std::string& last_error() const noexcept { return last_error_; }
    const MqttConfig& config() const noexcept { return cfg_; }
    std::uint64_t published() const noexcept { return published_; }
    std::uint64_t send_failures() const noexcept { return send_failures_; }

private:
    void teardown();

    MqttConfig cfg_;
    void* handle_ = nullptr;   // MQTTClient（避免在头文件里暴露 Paho 类型）
    bool connected_ = false;
    bool link_available_ = true;
    std::string last_error_;
    std::uint64_t published_ = 0;
    std::uint64_t send_failures_ = 0;
};

}  // namespace gw

#endif  // GW_MQTT_TRANSPORT_H
