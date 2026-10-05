#include "gw/mqtt_transport.h"

#include <cstring>

// Paho 只出现在这个 .cpp 里 —— 对外接口不含任何 Paho 类型
extern "C" {
#include "MQTTClient.h"
}

namespace gw {
namespace {

void put_u64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) {
        v.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 0xFFu));
    }
}
void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
}
std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t x = 0;
    for (int i = 0; i < 8; ++i) {
        x = (x << 8) | p[i];
    }
    return x;
}
std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

}  // namespace

// ── RecordCodec ─────────────────────────────────────────────────────────────
std::vector<std::uint8_t> RecordCodec::encode(const Record& rec) {
    if (rec.points.empty() || rec.points.size() > kMaxPoints) {
        return {};   // 空批或超限：返回空，由调用方判为失败
    }
    std::vector<std::uint8_t> v;
    v.reserve(kHeaderBytes + rec.points.size() * kPointBytes);
    v.push_back(kMagic0);
    v.push_back(kMagic1);
    v.push_back(kVersion);
    v.push_back(0);  // 保留
    put_u64(v, rec.seq);
    put_u64(v, rec.ts_ms);
    put_u16(v, static_cast<std::uint16_t>(rec.points.size()));
    for (const Point& p : rec.points) {
        put_u16(v, p.addr);
        put_u16(v, p.value);
    }
    return v;
}

bool RecordCodec::decode(const std::uint8_t* data, std::size_t len, Record& out) {
    if (data == nullptr || len < kHeaderBytes) {
        return false;
    }
    if (data[0] != kMagic0 || data[1] != kMagic1 || data[2] != kVersion) {
        return false;
    }
    const std::uint16_t count = get_u16(data + 20);
    if (count == 0 || count > kMaxPoints) {
        return false;
    }
    if (len != kHeaderBytes + static_cast<std::size_t>(count) * kPointBytes) {
        return false;   // 长度与点位数不自洽
    }
    out.seq = get_u64(data + 4);
    out.ts_ms = get_u64(data + 12);
    out.points.clear();
    out.points.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* p = data + kHeaderBytes + static_cast<std::size_t>(i) * kPointBytes;
        out.points.push_back(Point{get_u16(p), get_u16(p + 2)});
    }
    return true;
}

// ── MqttTransport ───────────────────────────────────────────────────────────
MqttTransport::MqttTransport(MqttConfig cfg) : cfg_(std::move(cfg)) {}

MqttTransport::~MqttTransport() {
    disconnect();
}

bool MqttTransport::is_connected() {
    if (handle_ == nullptr) {
        return false;
    }
    return MQTTClient_isConnected(static_cast<MQTTClient>(handle_)) != 0;
}

bool MqttTransport::connect() {
    if (handle_ == nullptr) {
        const std::string address = "tcp://" + cfg_.host + ":" + std::to_string(cfg_.port);
        MQTTClient h = nullptr;
        // 用持久化 = NONE：本次验证不依赖 Paho 的本地重发缓存，
        // 「断网续传」由我们自己的 RecordStore + 状态机负责（那才是要验证的东西）。
        const int rc = MQTTClient_create(&h, address.c_str(), cfg_.client_id.c_str(),
                                         MQTTCLIENT_PERSISTENCE_NONE, nullptr);
        if (rc != MQTTCLIENT_SUCCESS) {
            last_error_ = "MQTTClient_create failed rc=" + std::to_string(rc);
            return false;
        }
        handle_ = h;
    }

    if (is_connected()) {
        connected_ = true;
        return true;
    }

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.keepAliveInterval = cfg_.keep_alive_s;
    opts.cleansession = 1;
    opts.connectTimeout = 5;
    if (!cfg_.username.empty()) {
        opts.username = cfg_.username.c_str();
        opts.password = cfg_.password.c_str();
    }

    const int rc = MQTTClient_connect(static_cast<MQTTClient>(handle_), &opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        last_error_ = "connect failed rc=" + std::to_string(rc);
        connected_ = false;
        return false;
    }
    connected_ = true;
    return true;
}

void MqttTransport::teardown() {
    if (handle_ != nullptr) {
        // timeout=0：不等，直接拆。对已经死掉的 socket，等只会白花时间。
        MQTTClient_disconnect(static_cast<MQTTClient>(handle_), 0);
        MQTTClient_destroy(reinterpret_cast<MQTTClient*>(&handle_));
        handle_ = nullptr;
    }
    connected_ = false;
}

void MqttTransport::disconnect() {
    teardown();
}

void MqttTransport::set_link_available(bool up) {
    link_available_ = up;
    if (!up) {
        disconnect();   // 链路 down ⇒ 连接必然失效，主动释放socket
    }
}

SendResult MqttTransport::send(const Record& rec) {
    // 上层已明确告知链路不可用：不浪费一次网络往返，直接让状态机去退避
    if (!link_available_) {
        ++send_failures_;
        return SendResult::Retry;
    }

    // 惰性重连：外层状态机决定「何时尝试」；这里只负责「尝试就尽量连上」。
    if (!is_connected() && !connect()) {
        ++send_failures_;
        return SendResult::Retry;
    }

    const std::vector<std::uint8_t> payload = RecordCodec::encode(rec);
    if (payload.empty()) {
        // 正常情况下不该发生：DataProxy 保证每个 Record 的点位数都在编码器上限内。
        // 真发生了就记错误并 Retry（会表现为排空卡住，可见，不会静默丢数据）。
        last_error_ = "encode failed: 空批或点位数超限";
        ++send_failures_;
        return SendResult::Retry;
    }

    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = const_cast<std::uint8_t*>(payload.data());
    msg.payloadlen = static_cast<int>(payload.size());
    msg.qos = cfg_.qos;
    msg.retained = 0;

    MQTTClient_deliveryToken token = 0;
    int rc = MQTTClient_publishMessage(static_cast<MQTTClient>(handle_), cfg_.topic.c_str(), &msg,
                                       &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        last_error_ = "publish failed rc=" + std::to_string(rc);
        ++send_failures_;
        teardown();   // 连接不可信，拆掉让下次重建
        return SendResult::Retry;
    }

    // QoS1：等到 PUBACK 才算真正送达。超时即视为链路不可用 ——
    // 这条超时是必需的，因为 broker 被突然杀掉时 TCP 可能还「看起来」是连着的。
    rc = MQTTClient_waitForCompletion(static_cast<MQTTClient>(handle_), token,
                                      static_cast<unsigned long>(cfg_.ack_timeout_ms));
    if (rc != MQTTCLIENT_SUCCESS) {
        last_error_ = "waitForCompletion failed rc=" + std::to_string(rc);
        ++send_failures_;
        // ★ 关键：等到 PUBACK 超时，说明这条连接已经不可信。
        //   必须主动拆掉 —— 否则 paho 的 isConnected() 可能长期返回真（它要等到
        //   下一次 IO 才发现 socket 死了），我们就永远不会重连，状态机卡在 Backfill 空转。
        //   这个坑是实测踩出来的：broker 恢复后自检一直不收敛。
        teardown();
        return SendResult::Retry;
    }

    ++published_;
    return SendResult::Ok;
}

}  // namespace gw
