#include "gw/client.h"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace gw {
namespace {

constexpr std::size_t kReadBuf = 2048;

// 设置接收超时：Windows 用 DWORD 毫秒，POSIX 用 timeval
void set_recv_timeout(asio::ip::tcp::socket& sock, std::uint32_t ms) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(ms);
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

}  // namespace

// asio 的 socket 藏在实现里，头文件保持零依赖
struct NgwpClient::Impl {
    asio::io_context io;
    asio::ip::tcp::socket sock{io};
    std::array<std::uint8_t, kReadBuf> buf{};
};

NgwpClient::NgwpClient(ClientConfig cfg) : cfg_(std::move(cfg)), impl_(new Impl()) {}

NgwpClient::~NgwpClient() {
    close();
    delete static_cast<Impl*>(impl_);
}

void NgwpClient::set_error(const std::string& what) {
    last_error_ = what;
}

void NgwpClient::close() {
    auto* im = static_cast<Impl*>(impl_);
    if (im != nullptr) {
        asio::error_code ec;
        im->sock.close(ec);
    }
    open_ = false;
    parser_.reset();
}

bool NgwpClient::connect() {
    auto* im = static_cast<Impl*>(impl_);
    if (im == nullptr) {
        set_error("内部错误：impl 为空");
        return false;
    }
    if (open_) {
        return true;
    }

    asio::error_code ec;
    im->sock.close(ec);   // 清掉可能的旧状态
    im->sock = asio::ip::tcp::socket(im->io);

    const asio::ip::tcp::endpoint ep(asio::ip::make_address(cfg_.host), cfg_.port);
    // ★ 必须先 open 再 non_blocking —— 对未打开的 socket 调 non_blocking 会抛
    //   system_error("提供的文件句柄无效")。这个坑实测踩到过。
    im->sock.open(ep.protocol(), ec);
    if (ec) {
        set_error("open: " + ec.message());
        return false;
    }
    // 连接超时：asio 同步 connect 没有超时参数，这里用非阻塞 + 轮询实现
    im->sock.non_blocking(true, ec);
    if (ec) {
        set_error("non_blocking: " + ec.message());
        return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.connect_timeout_ms);
    for (;;) {
        im->sock.connect(ep, ec);
        if (!ec) {
            break;
        }
        if (ec != asio::error::in_progress && ec != asio::error::would_block &&
            ec != asio::error::already_started) {
            set_error("connect: " + ec.message());
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            set_error("connect 超时");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    im->sock.non_blocking(false);
    set_recv_timeout(im->sock, cfg_.timeout_ms);

    parser_.reset();
    open_ = true;
    return true;
}

void NgwpClient::reconfigure(const ClientConfig& cfg) {
    close();
    cfg_ = cfg;
    // 重连次数不清零：它是这条"会话生命期"的统计，跨重配置仍然有意义
}

bool NgwpClient::ensure_open() {
    if (open_) {
        return true;
    }
    const bool was_ever_open = (requests_ok_ > 0 || requests_failed_ > 0);
    if (!connect()) {
        return false;
    }
    if (was_ever_open) {
        ++reconnects_;   // 断线后的重建
    }
    return true;
}

bool NgwpClient::transact(const Frame& req, Frame& resp) {
    if (!ensure_open()) {
        ++requests_failed_;
        return false;
    }
    auto* im = static_cast<Impl*>(impl_);

    const std::vector<std::uint8_t> wire = encode(req);
    asio::error_code ec;
    asio::write(im->sock, asio::buffer(wire), ec);
    if (ec) {
        set_error("write: " + ec.message());
        ++requests_failed_;
        close();   // 连接不可信，下次重建（与 MQTT 传输同样的教训）
        return false;
    }

    for (;;) {
        const std::size_t n = im->sock.read_some(asio::buffer(im->buf), ec);
        if (ec) {
            set_error("read: " + ec.message());
            ++requests_failed_;
            close();
            return false;
        }
        for (const ParseEvent& ev : parser_.feed(im->buf.data(), n)) {
            if (const auto* d = std::get_if<DecodedFrame>(&ev)) {
                resp = d->frame;
                ++requests_ok_;
                return true;
            }
            // 解析错误（噪声/坏帧）不立刻失败：继续等有效帧或超时
        }
    }
}

std::vector<std::uint16_t> NgwpClient::decode_u16(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint16_t> out;
    out.reserve(payload.size() / 2);
    for (std::size_t i = 0; i + 1 < payload.size(); i += 2) {
        out.push_back(static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(payload[i]) << 8) |
            static_cast<std::uint16_t>(payload[i + 1])));
    }
    return out;
}

namespace {

// 统一处理「异常响应」：记录异常码并让便捷接口返回 false
bool take_ok(const Frame& resp, bool& had, ExceptionCode& code) {
    if (resp.is_exception()) {
        had = true;
        if (!resp.payload.empty()) {
            code = static_cast<ExceptionCode>(resp.payload[0]);
        }
        return false;
    }
    return true;
}

}  // namespace

bool NgwpClient::read_holding(std::uint16_t addr, std::uint16_t qty,
                              std::vector<std::uint16_t>& out) {
    Frame resp;
    if (!transact(make_read_request(static_cast<std::uint8_t>(Func::kReadHolding), addr, qty),
                  resp)) {
        return false;
    }
    if (!take_ok(resp, had_exception_, last_exception_)) {
        return false;
    }
    out = decode_u16(resp.payload);
    return out.size() == qty;
}

bool NgwpClient::read_input(std::uint16_t addr, std::uint16_t qty,
                            std::vector<std::uint16_t>& out) {
    Frame resp;
    if (!transact(make_read_request(static_cast<std::uint8_t>(Func::kReadInput), addr, qty), resp)) {
        return false;
    }
    if (!take_ok(resp, had_exception_, last_exception_)) {
        return false;
    }
    out = decode_u16(resp.payload);
    return out.size() == qty;
}

bool NgwpClient::write_single(std::uint16_t addr, std::uint16_t value) {
    Frame resp;
    if (!transact(make_write_single_request(addr, value), resp)) {
        return false;
    }
    return take_ok(resp, had_exception_, last_exception_);
}

bool NgwpClient::read_identity(std::string& out) {
    Frame resp;
    if (!transact(make_read_request(static_cast<std::uint8_t>(Func::kReadIdentity), 0, 0), resp)) {
        return false;
    }
    if (!take_ok(resp, had_exception_, last_exception_)) {
        return false;
    }
    out.assign(resp.payload.begin(), resp.payload.end());
    return true;
}

}  // namespace gw
