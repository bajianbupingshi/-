#ifndef GW_TESTING_MINI_MQTT_BROKER_H
#define GW_TESTING_MINI_MQTT_BROKER_H

// ⚠️ 仅供测试 / 自检使用的**最小 MQTT 3.1.1 broker**。不要用于生产。
//
// 存在的理由：验证「断网续传跑在真 MQTT 上」时，手上不一定有 docker/NanoMQ；
// 而这件事必须能在本机、在 CI 里自动跑。所以写一个真讲 MQTT 线格式的对端：
//   · 真 TCP socket、真 MQTT 帧（CONNECT/CONNACK、PUBLISH+PUBACK、SUBSCRIBE+SUBACK、PINGREQ/RESP）
//   · 它同时是**接收端对账器**（Ledger）——「丢失 0」由它来数，而不是发布端自己说
//   · 支持 stop()/start()：关掉监听并断开所有连接 = 制造一次真实断网，可再恢复
//
// 相比真 broker 的简化（已在文档中声明）：
//   · 只支持 QoS0/QoS1，不支持 QoS2、不支持保留消息、不支持遗嘱
//   · 订阅投递用 QoS0（本次验证的是发布侧 QoS1 的确认语义与续传行为）
//   · 主题匹配只支持精确匹配，不支持通配符
//   · 不支持持久会话（CONNACK 里 session-present 恒为 0）
#include <asio.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "gw/mqtt_transport.h"
#include "gw/reliability.h"

namespace gw {
namespace testing {

class MiniMqttBroker;

namespace detail {
inline std::uint16_t rd_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}
}  // namespace detail

// 一个客户端连接
class MiniMqttBrokerSession : public std::enable_shared_from_this<MiniMqttBrokerSession> {
public:
    MiniMqttBrokerSession(asio::ip::tcp::socket sock, MiniMqttBroker& broker)
        : sock_(std::move(sock)), broker_(broker) {}

    void start() { do_read(); }

    void close_now() {
        asio::error_code ec;
        sock_.close(ec);
    }

    bool subscribed_to(const std::string& topic) const {
        for (const std::string& s : subs_) {
            if (s == topic) {
                return true;
            }
        }
        return false;
    }

    void send_publish(const std::string& topic, const std::vector<std::uint8_t>& payload);

private:
    void do_read();
    void process_buffer();
    void handle_packet(std::uint8_t type, std::size_t start, std::size_t len);
    void send_bytes(std::vector<std::uint8_t> bytes);
    void do_write();

    asio::ip::tcp::socket sock_;
    MiniMqttBroker& broker_;
    std::array<std::uint8_t, 4096> raw_{};
    std::vector<std::uint8_t> buf_;
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> wq_;
    std::vector<std::string> subs_;
};

class MiniMqttBroker {
public:
    explicit MiniMqttBroker(asio::io_context& io) : acceptor_(io) {}

    ~MiniMqttBroker() { stop(); }

    // port = 0 表示由内核分配空闲端口
    bool start(std::uint16_t port = 0) {
        if (running_) {
            return true;
        }
        asio::error_code ec;
        acceptor_.open(asio::ip::tcp::v4(), ec);
        if (ec) {
            return false;
        }
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        acceptor_.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port), ec);
        if (ec) {
            return false;
        }
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            return false;
        }
        running_ = true;
        do_accept();
        return true;
    }

    // 关监听 + 断开所有连接 = 制造一次真实断网。可用 start() 恢复。
    void stop() {
        if (!running_ && sessions_.empty()) {
            return;
        }
        running_ = false;
        asio::error_code ec;
        acceptor_.close(ec);
        for (auto& weak : sessions_) {
            if (auto s = weak.lock()) {
                s->close_now();
            }
        }
        sessions_.clear();
    }

    std::uint16_t port() const {
        asio::error_code ec;
        const asio::ip::tcp::endpoint ep = acceptor_.local_endpoint(ec);
        return ec ? 0 : ep.port();
    }

    bool running() const noexcept { return running_; }

    // ── 接收端对账（「丢失 0」由这里数出来）──────────────────────────────────
    const Ledger& ledger() const noexcept { return ledger_; }
    std::uint64_t publishes() const noexcept { return publishes_; }
    std::uint64_t bad_payloads() const noexcept { return bad_payloads_; }
    std::uint64_t duplicate_publishes() const noexcept { return duplicate_publishes_; }
    std::uint64_t connects() const noexcept { return connects_; }
    std::uint64_t delivered() const noexcept { return delivered_; }
    std::uint64_t active_connections() const noexcept { return sessions_.size(); }

    // ── 供 Session 回调 ─────────────────────────────────────────────────────
    void on_connect() { ++connects_; }

    void on_publish(const std::string& topic, const std::uint8_t* payload, std::size_t len) {
        ++publishes_;
        last_topic_ = topic;
        // broker 的本分是**路由一切**载荷 —— 不能只投递本项目的二进制数据格式。
        // （W4-1 告警发布实测踩到：JSON 告警在 decode 失败处被丢弃，订阅端
        //   永远收不到。broker 只对「数据」对账，非数据载荷计数即可。）
        Record rec;
        const bool is_data = RecordCodec::decode(payload, len, rec);
        if (!is_data) {
            ++bad_payloads_;
        }
        deliver(topic, payload, len);
        if (is_data) {
            if (ledger_.has(rec.seq)) {
                ++duplicate_publishes_;
            }
            ledger_.accept(rec);
        }
    }

    void register_session(const std::shared_ptr<MiniMqttBrokerSession>& s) {
        sessions_.push_back(s);
    }

private:
    void do_accept() {
        if (!running_) {
            return;
        }
        acceptor_.async_accept([this](const asio::error_code& ec, asio::ip::tcp::socket sock) {
            if (!ec) {
                auto s = std::make_shared<MiniMqttBrokerSession>(std::move(sock), *this);
                register_session(s);
                s->start();
            }
            do_accept();
        });
    }

    void deliver(const std::string& topic, const std::uint8_t* payload, std::size_t len) {
        std::vector<std::uint8_t> copy(payload, payload + len);
        // 复制一份 weak 列表再遍历：投递过程中可能有关闭
        const std::vector<std::weak_ptr<MiniMqttBrokerSession>> snapshot = sessions_;
        for (const auto& weak : snapshot) {
            if (auto s = weak.lock()) {
                if (s->subscribed_to(topic)) {
                    s->send_publish(topic, copy);
                    ++delivered_;
                }
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    bool running_ = false;
    Ledger ledger_;
    std::uint64_t publishes_ = 0;
    std::uint64_t bad_payloads_ = 0;
    std::uint64_t duplicate_publishes_ = 0;
    std::uint64_t connects_ = 0;
    std::uint64_t delivered_ = 0;
    std::string last_topic_;
    std::vector<std::weak_ptr<MiniMqttBrokerSession>> sessions_;
};

// ── 会话实现（依赖 MiniMqttBroker 的完整定义，故放在后面）────────────────────
inline void MiniMqttBrokerSession::send_bytes(std::vector<std::uint8_t> bytes) {
    const bool idle = wq_.empty();
    wq_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(bytes)));
    if (idle) {
        do_write();
    }
}

inline void MiniMqttBrokerSession::do_write() {
    auto self = shared_from_this();
    asio::async_write(sock_, asio::buffer(*wq_.front()),
                      [this, self](const asio::error_code& ec, std::size_t) {
                          if (ec) {
                              return;
                          }
                          wq_.pop_front();
                          if (!wq_.empty()) {
                              do_write();
                          }
                      });
}

inline void MiniMqttBrokerSession::send_publish(const std::string& topic,
                                                const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    const std::size_t remaining = 2 + topic.size() + payload.size();
    out.push_back(0x30);  // PUBLISH, QoS0
    // remaining length 变长编码
    std::size_t x = remaining;
    do {
        std::uint8_t digit = static_cast<std::uint8_t>(x % 128);
        x /= 128;
        if (x > 0) {
            digit |= 0x80;
        }
        out.push_back(digit);
    } while (x > 0);
    out.push_back(static_cast<std::uint8_t>((topic.size() >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(topic.size() & 0xFF));
    out.insert(out.end(), topic.begin(), topic.end());
    out.insert(out.end(), payload.begin(), payload.end());
    send_bytes(std::move(out));
}

inline void MiniMqttBrokerSession::do_read() {
    auto self = shared_from_this();
    sock_.async_read_some(asio::buffer(raw_), [this, self](const asio::error_code& ec,
                                                           std::size_t n) {
        if (ec || n == 0) {
            return;  // 连接结束
        }
        buf_.insert(buf_.end(), raw_.data(), raw_.data() + n);
        process_buffer();
        do_read();
    });
}

inline void MiniMqttBrokerSession::process_buffer() {
    for (;;) {
        if (buf_.size() < 2) {
            return;
        }
        const std::uint8_t type = static_cast<std::uint8_t>(buf_[0] >> 4);
        std::size_t mult = 1;
        std::size_t value = 0;
        std::size_t pos = 1;
        std::uint8_t digit = 0;
        do {
            if (pos >= buf_.size()) {
                return;  // 变长长度还没收全
            }
            digit = buf_[pos++];
            value += static_cast<std::size_t>(digit & 0x7F) * mult;
            mult *= 128;
        } while ((digit & 0x80) != 0);

        if (buf_.size() < pos + value) {
            return;  // 载荷没到齐
        }
        handle_packet(type, pos, value);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos + value));
    }
}

inline void MiniMqttBrokerSession::handle_packet(std::uint8_t type, std::size_t start,
                                                 std::size_t len) {
    const std::uint8_t flags = static_cast<std::uint8_t>(buf_[0] & 0x0F);
    switch (type) {
        case 0x1: {  // CONNECT
            broker_.on_connect();
            // CONNACK：session-present=0, return-code=0
            send_bytes({0x20, 0x02, 0x00, 0x00});
            break;
        }
        case 0x3: {  // PUBLISH
            const std::uint8_t qos = static_cast<std::uint8_t>((flags >> 1) & 0x03);
            std::size_t off = start;
            if (off + 2 > start + len) {
                break;
            }
            const std::uint16_t tlen = detail::rd_u16(buf_.data() + off);
            off += 2;
            if (off + tlen > start + len) {
                break;
            }
            const std::string topic(reinterpret_cast<const char*>(buf_.data() + off), tlen);
            off += tlen;
            std::uint16_t pid = 0;
            if (qos > 0) {
                if (off + 2 > start + len) {
                    break;
                }
                pid = detail::rd_u16(buf_.data() + off);
                off += 2;
            }
            const std::uint8_t* payload = buf_.data() + off;
            const std::size_t plen = (start + len) - off;
            broker_.on_publish(topic, payload, plen);
            if (qos > 0) {  // PUBACK
                send_bytes({0x40, 0x02, static_cast<std::uint8_t>((pid >> 8) & 0xFF),
                            static_cast<std::uint8_t>(pid & 0xFF)});
            }
            break;
        }
        case 0x8: {  // SUBSCRIBE
            std::size_t off = start;
            if (off + 2 > start + len) {
                break;
            }
            const std::uint16_t pid = detail::rd_u16(buf_.data() + off);
            off += 2;
            std::vector<std::uint8_t> granted;
            while (off < start + len) {
                if (off + 2 > start + len) {
                    break;
                }
                const std::uint16_t flen = detail::rd_u16(buf_.data() + off);
                off += 2;
                if (off + flen > start + len) {
                    break;
                }
                subs_.emplace_back(reinterpret_cast<const char*>(buf_.data() + off), flen);
                off += flen;
                if (off < start + len) {
                    ++off;  // 请求的 QoS 字节
                }
                granted.push_back(0x00);  // 一律授予 QoS0
            }
            std::vector<std::uint8_t> suback{0x90, static_cast<std::uint8_t>(2 + granted.size()),
                                             static_cast<std::uint8_t>((pid >> 8) & 0xFF),
                                             static_cast<std::uint8_t>(pid & 0xFF)};
            suback.insert(suback.end(), granted.begin(), granted.end());
            send_bytes(std::move(suback));
            break;
        }
        case 0xC:    // PINGREQ
            send_bytes({0xD0, 0x00});
            break;
        case 0xE:    // DISCONNECT
            close_now();
            break;
        default:
            break;   // 其余类型（QoS2 等）本最小实现不支持，直接忽略
    }
}

}  // namespace testing
}  // namespace gw

#endif  // GW_TESTING_MINI_MQTT_BROKER_H
