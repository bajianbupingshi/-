// NGWP 设备模拟器（asio standalone，C++17）
//
// 这是方案里的「自研件 ①」：一台用自定义类 HSMS 协议对外提供寄存器读写的设备。
// 它把 W1 已经验证过的协议层（FrameParser / encode / DeviceModel）接到真实 TCP 上，
// 自身只负责「收字节、喂解析器、把响应当字节发回去」。
//
// 线程模型：**单线程 io_context** —— 因此 DeviceModel 与寄存器表无需加锁。
// 要上多线程，必须先给每个会话加 strand 或给模型加锁（README 里已注明）。
//
// 用法：
//   gw_sim                                  # 默认 0.0.0.0:1502，12 点位，100ms 一个采样周期
//   gw_sim --port 1502 --points 12 --interval 100 --seed 20261005
//   gw_sim --selftest                       # 进程内起客户端，跑端到端自检后退出
// ASIO_STANDALONE 由 CMake 传入；这里兜底，避免重复定义（-Werror 会抓 redefine）
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gw/client.h"
#include "gw/device_model.h"
#include "gw/frame.h"
#include "gw/parser.h"

namespace {

using asio::ip::tcp;

// ── 选项 ────────────────────────────────────────────────────────────────────
struct Options {
    std::string bind = "0.0.0.0";
    std::uint16_t port = 1502;
    std::size_t holding_count = 256;
    std::size_t input_count = 16;
    std::uint16_t points = 12;          // 正弦注入的点位数
    std::uint32_t interval_ms = 100;    // 采样周期
    std::uint64_t seed = 20261005;
    std::string identity = gw::kDefaultIdentity;
    bool selftest = false;
    bool client_selftest = false;
    bool verbose = false;
};

void usage() {
    std::printf(
        "用法: gw_sim [选项]\n"
        "  --bind <ip>        监听地址（默认 0.0.0.0）\n"
        "  --port <n>         监听端口（默认 1502）\n"
        "  --holding <n>      保持寄存器数量（默认 256）\n"
        "  --input <n>        输入寄存器数量（默认 16）\n"
        "  --points <n>       正弦注入点位数（默认 12）\n"
        "  --interval <ms>    采样周期（默认 100）\n"
        "  --seed <n>         确定性随机种子（默认 20261005）\n"
        "  --identity <s>     设备标识（READ_IDENTITY 返回）\n"
        "  --selftest         进程内端到端自检后退出（供 CI 使用）\n"
        "  --client-selftest  用 NgwpClient 对着进程内服务器跑客户端自检\n"
        "  --verbose          打印每帧收发\n"
        "  --help\n");
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("选项 %s 缺参数\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--bind") {
            opt.bind = need("--bind");
        } else if (a == "--port") {
            opt.port = static_cast<std::uint16_t>(std::strtoul(need("--port"), nullptr, 10));
        } else if (a == "--holding") {
            opt.holding_count = std::strtoul(need("--holding"), nullptr, 10);
        } else if (a == "--input") {
            opt.input_count = std::strtoul(need("--input"), nullptr, 10);
        } else if (a == "--points") {
            opt.points = static_cast<std::uint16_t>(std::strtoul(need("--points"), nullptr, 10));
        } else if (a == "--interval") {
            opt.interval_ms = static_cast<std::uint32_t>(std::strtoul(need("--interval"), nullptr, 10));
        } else if (a == "--seed") {
            opt.seed = std::strtoull(need("--seed"), nullptr, 10);
        } else if (a == "--identity") {
            opt.identity = need("--identity");
        } else if (a == "--selftest") {
            opt.selftest = true;
        } else if (a == "--client-selftest") {
            opt.client_selftest = true;
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            std::printf("未知选项: %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

// ── 一个客户端会话 ──────────────────────────────────────────────────────────
//
// 写队列是必须的：如果直接复用同一块发送缓冲，第二次读触发的写会覆盖
// 第一次尚未发完的缓冲 —— 那是典型的 use-after-free。这里用 deque 串行化。
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket sock, gw::DeviceModel& model, bool verbose)
        : sock_(std::move(sock)), model_(model), verbose_(verbose) {
        if (verbose_) {
            asio::error_code ec;
            const tcp::endpoint ep = sock_.remote_endpoint(ec);
            if (!ec) {
                std::printf("[sim] 会话接入 %s:%u\n", ep.address().to_string().c_str(), ep.port());
            }
        }
    }

    void start() { do_read(); }

    std::uint64_t frames_served() const noexcept { return frames_served_; }
    std::uint64_t parse_errors() const noexcept { return parse_errors_; }

private:
    void do_read() {
        auto self = shared_from_this();
        sock_.async_read_some(
            asio::buffer(buf_),
            [this, self](const asio::error_code& ec, std::size_t n) {
                if (ec) {
                    return;  // 对端关闭或出错：会话自然结束
                }
                const std::vector<gw::ParseEvent> events = parser_.feed(buf_.data(), n);

                std::vector<std::uint8_t> batch;
                for (const auto& e : events) {
                    if (const auto* decoded = std::get_if<gw::DecodedFrame>(&e)) {
                        const gw::Frame resp = model_.handle(decoded->frame);
                        const std::vector<std::uint8_t> wire = gw::encode(resp);
                        batch.insert(batch.end(), wire.begin(), wire.end());
                        ++frames_served_;
                        if (verbose_) {
                            std::printf("[sim] 收 %s → 回 %s\n", gw::to_string(e).c_str(),
                                        gw::to_string(gw::ParseEvent{gw::DecodedFrame{resp, 0}}).c_str());
                        }
                    } else {
                        // 坏字节不造响应：设备侧对畸形报文保持沉默（只计数）
                        ++parse_errors_;
                        if (verbose_) {
                            std::printf("[sim] 丢弃畸形片段：%s\n", gw::to_string(e).c_str());
                        }
                    }
                }
                if (!batch.empty()) {
                    enqueue(std::move(batch));
                }
                do_read();
            });
    }

    void enqueue(std::vector<std::uint8_t> data) {
        const bool idle = write_q_.empty();
        write_q_.push_back(std::make_shared<std::vector<std::uint8_t>>(std::move(data)));
        if (idle) {
            do_write();
        }
    }

    void do_write() {
        auto self = shared_from_this();
        asio::async_write(
            sock_, asio::buffer(*write_q_.front()),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (ec) {
                    return;
                }
                write_q_.pop_front();
                if (!write_q_.empty()) {
                    do_write();
                }
            });
    }

    tcp::socket sock_;
    gw::DeviceModel& model_;
    bool verbose_;
    gw::FrameParser parser_;
    std::array<std::uint8_t, 8192> buf_{};
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_q_;
    std::uint64_t frames_served_ = 0;
    std::uint64_t parse_errors_ = 0;
};

// ── 服务器 ──────────────────────────────────────────────────────────────────
class SimServer {
public:
    SimServer(asio::io_context& io, const Options& opt, std::uint16_t bind_port)
        : acceptor_(io),
          timer_(io),
          model_(opt.holding_count, opt.input_count, opt.identity, opt.seed),
          interval_ms_(opt.interval_ms),
          verbose_(opt.verbose) {
        // 点位布局：0..points-1 正弦（无状态）+ points 处一个随机游走
        //（混一种消耗 RNG 的策略，否则 seed 是摆设 —— 这个坑在 W1 的演示里踩过）
        auto bindings = gw::make_sine_bindings(0, opt.points, gw::Sine{30000.0, 8000.0, 64, 0.0});
        bindings.push_back(gw::RegisterBinding{
            opt.points, gw::RandomWalk{30000, 64, 0, 65535}});
        model_.set_bindings(std::move(bindings));

        const asio::ip::address addr = asio::ip::make_address(opt.bind);
        const tcp::endpoint endpoint(addr, bind_port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        do_accept();
        schedule_tick();
    }

    std::uint16_t port() const {
        asio::error_code ec;
        const tcp::endpoint ep = acceptor_.local_endpoint(ec);
        return ec ? 0 : ep.port();
    }

    gw::DeviceModel& model() noexcept { return model_; }
    std::uint64_t accepted() const noexcept { return accepted_; }

private:
    void do_accept() {
        acceptor_.async_accept([this](const asio::error_code& ec, tcp::socket sock) {
            if (!ec) {
                ++accepted_;
                std::make_shared<Session>(std::move(sock), model_, verbose_)->start();
            }
            do_accept();  // 接受失败也继续听（例如瞬时 fd 耗尽）
        });
    }

    void schedule_tick() {
        timer_.expires_after(std::chrono::milliseconds(interval_ms_));
        timer_.async_wait([this](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            model_.tick(tick_++);
            schedule_tick();
        });
    }

    tcp::acceptor acceptor_;
    asio::steady_timer timer_;
    gw::DeviceModel model_;
    std::uint32_t interval_ms_;
    bool verbose_;
    std::uint64_t tick_ = 0;
    std::uint64_t accepted_ = 0;
};

// ── 自检 ────────────────────────────────────────────────────────────────────
int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("    [FAIL] %s\n", what.c_str());
    }
}

// 同步收一批字节喂进解析器，直到凑够 want 帧
std::vector<gw::Frame> recv_frames(tcp::socket& sock, gw::FrameParser& parser, std::size_t want) {
    std::vector<gw::Frame> got;
    std::array<std::uint8_t, 4096> buf{};
    while (got.size() < want) {
        asio::error_code ec;
        const std::size_t n = sock.read_some(asio::buffer(buf), ec);
        if (ec || n == 0) {
            break;
        }
        for (const auto& e : parser.feed(buf.data(), n)) {
            if (const auto* d = std::get_if<gw::DecodedFrame>(&e)) {
                got.push_back(d->frame);
            }
        }
    }
    return got;
}

void send_bytes(tcp::socket& sock, const std::vector<std::uint8_t>& v) {
    asio::write(sock, asio::buffer(v));
}

int run_selftest(Options opt) {
    opt.bind = "127.0.0.1";
    opt.interval_ms = 600000;  // 自检期间不做定时注入，避免与断言争用寄存器表

    std::printf("=========== gw_sim 端到端自检 ===========\n");
    asio::io_context io;
    SimServer server(io, opt, /*bind_port=*/0);  // 0 → 由内核分配空闲端口
    std::thread th([&io] { io.run(); });

    // 看门狗：任何一步卡住都直接退出，避免 CI 挂死
    std::thread([&] {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        std::printf("    [FAIL] 自检超时（15s），强制退出\n");
        std::_Exit(3);
    }).detach();

    const tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), server.port());
    std::printf("  监听 127.0.0.1:%u（内核分配）\n", server.port());

    asio::io_context client_io;
    tcp::socket sock(client_io);
    sock.connect(ep);
    gw::FrameParser parser;

    // ① 单帧往返
    {
        send_bytes(sock, gw::encode(gw::make_read_request(0x01, 0, 4)));
        const std::vector<gw::Frame> r = recv_frames(sock, parser, 1);
        check(r.size() == std::size_t{1}, "① 单帧往返应收到 1 个响应");
        if (r.size() == 1) {
            check(!r[0].is_exception(), "① 响应不该是异常帧");
            check(r[0].qty == 4, "① 响应数量应为 4");
            check(r[0].payload.size() == 8, "① 响应载荷应为 8 字节");
            const std::vector<std::uint16_t> expect = server.model().table().read_block(0, 4, false);
            for (std::size_t i = 0; i < expect.size(); ++i) {
                const std::uint16_t got = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(r[0].payload[i * 2]) << 8) |
                    static_cast<std::uint16_t>(r[0].payload[i * 2 + 1]));
                check(got == expect[i], "① 第 " + std::to_string(i) + " 个值与设备寄存器一致");
            }
        }
    }

    // ② 拆包：逐字节发送，服务端仍应拼出完整一帧
    {
        const std::vector<std::uint8_t> wire = gw::encode(gw::make_read_request(0x01, 1, 2));
        for (std::uint8_t b : wire) {
            send_bytes(sock, std::vector<std::uint8_t>{b});
        }
        const std::vector<gw::Frame> r = recv_frames(sock, parser, 1);
        check(r.size() == std::size_t{1}, "② 逐字节发送仍应得到 1 个响应");
        if (r.size() == 1) {
            check(r[0].qty == 2, "② 响应数量应为 2");
        }
    }

    // ③ 粘包：一次发送两个请求，应得到两个响应
    {
        std::vector<std::uint8_t> two = gw::encode(gw::make_read_request(0x01, 0, 1));
        const std::vector<std::uint8_t> second = gw::encode(gw::make_read_request(0x02, 0, 1));
        two.insert(two.end(), second.begin(), second.end());
        send_bytes(sock, two);
        const std::vector<gw::Frame> r = recv_frames(sock, parser, 2);
        check(r.size() == std::size_t{2}, "③ 粘包应解出 2 个响应");
        if (r.size() == 2) {
            check(r[0].func_code() == std::uint8_t{0x01}, "③ 第 1 个响应功能码应为 0x01");
            check(r[1].func_code() == std::uint8_t{0x02}, "③ 第 2 个响应功能码应为 0x02");
        }
    }

    // ④ 写入生效
    {
        send_bytes(sock, gw::encode(gw::make_write_single_request(5, 0x1234)));
        const std::vector<gw::Frame> w = recv_frames(sock, parser, 1);
        check(w.size() == std::size_t{1} && !w[0].is_exception(), "④ 写单个应有正常响应");
        if (w.size() == 1) {
            check(w[0].payload.size() == 2 && w[0].payload[0] == 0x12 && w[0].payload[1] == 0x34,
                  "④ 写单个应回显写入值");
        }
        send_bytes(sock, gw::encode(gw::make_read_request(0x01, 5, 1)));
        const std::vector<gw::Frame> r = recv_frames(sock, parser, 1);
        check(r.size() == std::size_t{1}, "④ 写后读应得到 1 个响应");
        if (r.size() == 1) {
            check(r[0].payload[0] == 0x12 && r[0].payload[1] == 0x34, "④ 写后读的值应一致");
        }
        check(server.model().table().read(5, false) == 0x1234, "④ 服务端寄存器表应被更新");
    }

    // ⑤ 语义错误 → 异常帧（不是静默丢弃）
    {
        send_bytes(sock, gw::encode(gw::make_read_request(0x01, 60000, 1)));
        const std::vector<gw::Frame> r = recv_frames(sock, parser, 1);
        check(r.size() == std::size_t{1}, "⑤ 越界读应得到 1 个响应");
        if (r.size() == 1) {
            check(r[0].is_exception(), "⑤ 越界读应回异常帧");
            check(!r[0].payload.empty() &&
                      static_cast<gw::ExceptionCode>(r[0].payload[0]) ==
                          gw::ExceptionCode::kIllegalAddress,
                  "⑤ 异常码应为 ILLEGAL_ADDRESS");
        }
    }

    // ⑥ 坏帧重同步：坏 CRC 的帧丢弃，紧随其后的合法帧仍应被应答
    {
        std::vector<std::uint8_t> bad = gw::encode(gw::make_read_request(0x01, 0, 1));
        bad[8] ^= 0x01;  // 破坏 CRC 保护区内的一个字节
        std::vector<std::uint8_t> stream = bad;
        const std::vector<std::uint8_t> good = gw::encode(gw::make_read_request(0x01, 3, 1));
        stream.insert(stream.end(), good.begin(), good.end());
        send_bytes(sock, stream);
        const std::vector<gw::Frame> r = recv_frames(sock, parser, 1);
        check(r.size() == std::size_t{1}, "⑥ 坏帧+好帧只应得到 1 个响应（坏帧被静默丢弃）");
        if (r.size() == 1) {
            check(r[0].addr == 3, "⑥ 响应对应的应是那个好帧（addr=3）");
        }
    }

    asio::error_code ignored;
    sock.close(ignored);
    io.stop();
    th.join();

    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// ── 客户端自检：NgwpClient 对着进程内服务器 ─────────────────────────────────
//
// 意义：插件（Linux .so，本机编不了）要用的协议客户端这一层，在这里被完整验证。
// 于是将来插件出问题，就只剩「SDK / ABI」这一类原因，不会混进协议问题。
int run_client_selftest(Options opt) {
    opt.bind = "127.0.0.1";
    std::printf("=========== NgwpClient 客户端自检 ===========\n");

    asio::io_context io;
    SimServer server(io, opt, /*bind_port=*/0);
    std::thread th([&io] { io.run(); });
    const std::uint16_t port = server.port();

    std::thread([&] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::printf("    [FAIL] 自检超时（30s）\n");
        std::_Exit(3);
    }).detach();

    gw::ClientConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.timeout_ms = 1000;
    gw::NgwpClient client(cfg);
    std::printf("  服务器 127.0.0.1:%u\n", port);

    // ① 连接 + 读保持寄存器
    {
        std::vector<std::uint16_t> vals;
        check(client.read_holding(0, 4, vals), "① 读保持寄存器应成功");
        check(vals.size() == std::size_t{4}, "① 应读到 4 个值");
        const std::vector<std::uint16_t> expect = server.model().table().read_block(0, 4, false);
        check(vals == expect, "① 读到的值应与设备寄存器一致");
    }

    // ② 写单个 + 读回
    {
        check(client.write_single(9, 0xABCD), "② 写单个应成功");
        std::vector<std::uint16_t> vals;
        check(client.read_holding(9, 1, vals), "② 写后读应成功");
        check(!vals.empty() && vals[0] == 0xABCD, "② 写后读的值应一致");
        check(server.model().table().read(9, false) == 0xABCD, "② 设备侧寄存器应被更新");
    }

    // ③ 读设备标识
    {
        std::string id;
        check(client.read_identity(id), "③ 读标识应成功");
        check(id == opt.identity, "③ 标识应与服务器配置一致");
    }

    // ④ 语义错误：越界读 → 便捷接口返回 false 且能拿到异常码
    {
        client.clear_exception();
        std::vector<std::uint16_t> vals;
        const bool ok = client.read_holding(60000, 1, vals);
        check(!ok, "④ 越界读应返回 false");
        check(client.has_exception(), "④ 应记录到异常响应");
        check(client.last_exception() == gw::ExceptionCode::kIllegalAddress,
              "④ 异常码应为 ILLEGAL_ADDRESS");
    }

    // ⑤ 边界：qty 取上限 64（一次读满）
    {
        client.clear_exception();
        std::vector<std::uint16_t> vals;
        check(client.read_holding(0, gw::kMaxQty, vals), "⑤ qty=64 应成功");
        check(vals.size() == std::size_t{gw::kMaxQty}, "⑤ 应读满 64 个值");
    }

    // ⑥ 连续请求：验证真实 socket 上的粘包/半包处理
    {
        client.clear_exception();
        bool all_ok = true;
        for (int i = 0; i < 200; ++i) {
            std::vector<std::uint16_t> vals;
            if (!client.read_holding(static_cast<std::uint16_t>(i % 100), 2, vals) ||
                vals.size() != std::size_t{2}) {
                all_ok = false;
                break;
            }
        }
        check(all_ok, "⑥ 连续 200 次请求都应成功（粘包/半包处理正确）");
    }

    // ⑦ 断线重连：close 后下一次请求应自动重建连接
    {
        const std::uint64_t rc_before = client.reconnects();
        client.close();
        check(!client.is_open(), "⑦ close 后应处于未连接状态");
        std::vector<std::uint16_t> vals;
        check(client.read_holding(0, 1, vals), "⑦ close 后应能自动重连并成功");
        check(client.reconnects() > rc_before, "⑦ 重连次数应增加");
    }

    // ⑧ 连接失败要快速返回并留下原因（连到没人听的端口）
    {
        gw::NgwpClient bad(gw::ClientConfig{"127.0.0.1", 1, 300, 500});
        check(!bad.connect(), "⑧ 连到未监听端口应失败");
        check(!bad.last_error().empty(), "⑧ 应记录失败原因");
    }

    io.stop();
    th.join();
    std::printf("  请求成功 %llu / 失败 %llu / 重连 %llu\n",
                static_cast<unsigned long long>(client.requests_ok()),
                static_cast<unsigned long long>(client.requests_failed()),
                static_cast<unsigned long long>(client.reconnects()));
    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 2;
    }
    if (opt.selftest) {
        return run_selftest(opt);
    }
    if (opt.client_selftest) {
        return run_client_selftest(opt);
    }

    try {
        asio::io_context io;
        SimServer server(io, opt, opt.port);
        std::printf("NGWP 设备模拟器\n");
        std::printf("  监听      : %s:%u\n", opt.bind.c_str(), server.port());
        std::printf("  设备标识  : %s\n", opt.identity.c_str());
        std::printf("  寄存器    : 保持 %zu / 输入 %zu\n", opt.holding_count, opt.input_count);
        std::printf("  注入      : %u 个正弦点位 + 1 个随机游走，周期 %u ms，seed %llu\n",
                    static_cast<unsigned>(opt.points), opt.interval_ms,
                    static_cast<unsigned long long>(opt.seed));
        std::printf("  Ctrl-C 退出\n");

        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&io](const asio::error_code&, int) { io.stop(); });
        io.run();
        std::printf("\n已退出。\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("启动失败: %s\n", e.what());
        return 1;
    }
}
