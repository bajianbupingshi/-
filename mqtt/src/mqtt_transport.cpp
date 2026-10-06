#include "gw/mqtt_transport.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

// Paho 只出现在这个 .cpp 里 —— 对外接口不含任何 Paho 类型
extern "C" {
#include "MQTTAsync.h"
}

namespace gw {

// ── 投递完成槽（头文件里 forward 声明的嵌套类型，此处给出定义）─────────────
// 为什么存在：Paho C 1.3.13 的 MQTTAsync_waitForCompletion 内部就是
// 「sleep(100ms) + 轮询」循环（MQTTAsync.c）—— 同步/异步客户端共用这 100ms
// 粒度，这才是 R9 实测 ~116ms/条硬地板的真正来源（不是网络、也不只是同步
// 客户端）。正解：注册 onSuccess/onFailure 回调（PUBACK 到达即在 Paho 线程
// 触发），send() 用条件变量等它 —— 事件驱动唤醒，零轮询，
// ITransport 的「Ok = PUBACK 已确认」语义原封不动。
struct MqttTransport::DeliverySlot {
    std::mutex mtx;
    std::condition_variable cv;
    int state = 0;   // 0=等待中 1=已确认(PUBACK) 2=失败
};

namespace {

void on_sent_ok(void* context, MQTTAsync_successData* /*response*/) {
    auto* s = static_cast<MqttTransport::DeliverySlot*>(context);
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->state = 1;
    }
    s->cv.notify_all();
}

void on_sent_fail(void* context, MQTTAsync_failureData* /*response*/) {
    auto* s = static_cast<MqttTransport::DeliverySlot*>(context);
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->state = 2;
    }
    s->cv.notify_all();
}

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
//
// Paho MQTT C 的 **Async** API + 逐条 waitForCompletion。
//
// 为什么从同步 API（MQTTClient）换到这里：实测同步客户端单条发布 ~116 ms
// —— 它的 waitForCompletion 内部以 100ms 粒度轮询，是「硬地板」而非网络
// 慢（R9）。Async 客户端的后台线程收到 PUBACK 时用条件变量唤醒等待者，
// 同样的「发出去 → 等到 PUBACK → 返回」语义，等待精度从 100ms 级降到
// 毫秒级以下。ITransport 的对外语义一个字没改：Ok = PUBACK 已确认。
//
// 重连归属不变：仍由上层状态机控制重试节奏，不用 Paho 的自动重连；
// 持久化仍是 NONE（不让客户端缓存掩盖我们自己的队列行为）。
MqttTransport::MqttTransport(MqttConfig cfg) : cfg_(std::move(cfg)) {}

MqttTransport::~MqttTransport() {
    disconnect();
}

bool MqttTransport::is_connected() {
    if (handle_ == nullptr) {
        return false;
    }
    return MQTTAsync_isConnected(static_cast<MQTTAsync>(handle_)) != 0;
}

bool MqttTransport::connect() {
    if (handle_ == nullptr) {
        const std::string address = "tcp://" + cfg_.host + ":" + std::to_string(cfg_.port);
        MQTTAsync h = nullptr;
        // 用持久化 = NONE：本次验证不依赖 Paho 的本地重发缓存，
        // 「断网续传」由我们自己的 RecordStore + 状态机负责（那才是要验证的东西）。
        const int rc = MQTTAsync_create(&h, address.c_str(), cfg_.client_id.c_str(),
                                        MQTTCLIENT_PERSISTENCE_NONE, nullptr);
        if (rc != MQTTASYNC_SUCCESS) {
            last_error_ = "MQTTAsync_create failed rc=" + std::to_string(rc);
            return false;
        }
        handle_ = h;
    }

    if (is_connected()) {
        connected_ = true;
        return true;
    }

    MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
    opts.keepAliveInterval = cfg_.keep_alive_s;
    opts.cleansession = 1;
    opts.connectTimeout = 5;
    if (!cfg_.username.empty()) {
        opts.username = cfg_.username.c_str();
        opts.password = cfg_.password.c_str();
    }

    const int rc = MQTTAsync_connect(static_cast<MQTTAsync>(handle_), &opts);
    if (rc != MQTTASYNC_SUCCESS) {
        last_error_ = "connect failed rc=" + std::to_string(rc);
        connected_ = false;
        return false;
    }

    // 异步连接：SUCCESS 只代表请求已受理，CONNACK 到达后 isConnected 才翻真。
    // 这里轮询等它（本地 broker 亚毫秒级），等到才算「连上」—— 与同步语义对齐，
    // 上层状态机的时间线不因此改变。超时则拆掉句柄，下次重试从零开始。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(cfg_.ack_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (is_connected()) {
            connected_ = true;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    last_error_ = "connect 超时（CONNACK 未在时限内到达）";
    connected_ = false;
    teardown();
    return false;
}

void MqttTransport::teardown() {
    if (handle_ != nullptr) {
        // timeout=0：不等，直接拆。对已经死掉的 socket，等只会白花时间。
        MQTTAsync_disconnect(static_cast<MQTTAsync>(handle_), 0);
        MQTTAsync_destroy(reinterpret_cast<MQTTAsync*>(&handle_));
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

    // MQTTAsync_send 是「裸参数」版本；1.3.13 没有 MQTTAsync_publish（那是更新版本
    // 才有的）。token 从 responseOptions 里拿；onSuccess/onFailure 挂到完成槽 ——
    // PUBACK 到达时 Paho 线程置状态并唤醒，send() 的等待是事件驱动而非轮询。
    slot_ = std::make_shared<DeliverySlot>();
    MQTTAsync_responseOptions resp = MQTTAsync_responseOptions_initializer;
    resp.onSuccess = &on_sent_ok;
    resp.onFailure = &on_sent_fail;
    resp.context   = slot_.get();
    int rc = MQTTAsync_send(static_cast<MQTTAsync>(handle_), cfg_.topic.c_str(),
                            static_cast<int>(payload.size()),
                            const_cast<std::uint8_t*>(payload.data()), cfg_.qos, 0, &resp);
    if (rc != MQTTASYNC_SUCCESS) {
        last_error_ = "publish failed rc=" + std::to_string(rc);
        ++send_failures_;
        teardown();   // 连接不可信，拆掉让下次重建
        return SendResult::Retry;
    }
    const MQTTAsync_token token = resp.token;
    (void)token;   // 完成与否由回调通知；token 仅在调试时有用

    // QoS1：等到 PUBACK 才算真正送达。超时即视为链路不可用 ——
    // 这条超时是必需的，因为 broker 被突然杀掉时 TCP 可能还「看起来」是连着的。
    // 等待本身是事件驱动的（回调线程 notify_all），不受 Paho waitForCompletion
    // 内部 100ms 轮询粒度的影响。
    bool acked = false;
    {
        std::unique_lock<std::mutex> lk(slot_->mtx);
        acked = slot_->cv.wait_for(lk, std::chrono::milliseconds(cfg_.ack_timeout_ms),
                                   [&] { return slot_->state != 0; });
    }
    if (!acked || slot_->state == 2) {
        last_error_ = acked ? "PUBACK 失败（onFailure）" : "waitForCompletion 超时";
        ++send_failures_;
        // ★ 关键：等到 PUBACK 超时，说明这条连接已经不可信。
        //   必须主动拆掉 —— 否则 paho 的 isConnected() 可能长期返回真（它要等到
        //   下一次 IO 才发现 socket 死了），我们就永远不会重连，状态机卡在 Backfill 空转。
        //   这个坑在同步 API 时代实测踩过，Async 路线同样适用。
        teardown();
        return SendResult::Retry;
    }

    ++published_;
    return SendResult::Ok;
}

}  // namespace gw
