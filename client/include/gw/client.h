#ifndef GW_CLIENT_H
#define GW_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>

#include "gw/frame.h"
#include "gw/parser.h"

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// NGWP 同步客户端 —— 设备侧的对话方。
//
// 谁在用：Neuron 驱动插件（plugins/driver）用它跟设备通信。
// 为什么单独成 target：插件是 Linux 上的 .so，但「协议客户端」这层是纯 C++ +
// 一个 socket，可以在本机（Windows/MinGW）对着 gw_sim 完整验证 ——
// 这样插件的失败就只剩「SDK/ABI 问题」，不会再混进协议问题。
//
// 设计取舍：
//  · **同步**语义。插件被 Neuron 以 group_timer 周期回调，每次读一组点位，
//    同步请求-应答最直观；不需要事件循环。
//  · 一个连接一个实例，**非线程安全**（与 FrameParser 一致）。
//  · 超时用 SO_RCVTIMEO 实现（Windows 传 DWORD 毫秒、POSIX 传 timeval），
//    不引入 asio 的异步超时机制。
//  · 应答帧里 `func & 0x80` 表示异常响应：transact() 仍返回 true（帧合法），
//    由 read_* 这类便捷函数判断并转成 false —— 调用方不该被异常码淹没。
// ─────────────────────────────────────────────────────────────────────────────

struct ClientConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 1502;
    std::uint32_t timeout_ms = 1000;
    std::uint32_t connect_timeout_ms = 2000;
};

class NgwpClient {
public:
    explicit NgwpClient(ClientConfig cfg);
    NgwpClient(const NgwpClient&) = delete;
    NgwpClient& operator=(const NgwpClient&) = delete;
    ~NgwpClient();

    // 幂等；失败时 last_error() 有原因。断线后再次调用会重建连接。
    bool connect();
    void close();
    bool is_open() const noexcept { return open_; }

    // 改配置（会先关闭旧连接）。插件在 start() 时用 setting() 收到的参数重建客户端。
    void reconfigure(const ClientConfig& cfg);

    // 发一帧、等一帧（带超时）。成功收到**合法帧**即返回 true，
    // 该帧可能是异常响应（resp.is_exception()）。
    bool transact(const Frame& req, Frame& resp);

    // ── 便捷语义接口（异常响应会返回 false，并把异常码放进 last_exception()）──
    bool read_holding(std::uint16_t addr, std::uint16_t qty, std::vector<std::uint16_t>& out);
    bool read_input(std::uint16_t addr, std::uint16_t qty, std::vector<std::uint16_t>& out);
    bool write_single(std::uint16_t addr, std::uint16_t value);
    bool read_identity(std::string& out);

    const std::string& last_error() const noexcept { return last_error_; }
    // 最近一次收到的异常响应码（无则返回 kDeviceFailure 之外的哨兵：payload 空时有 has_exception() 判断）
    bool has_exception() const noexcept { return had_exception_; }
    ExceptionCode last_exception() const noexcept { return last_exception_; }
    void clear_exception() noexcept { had_exception_ = false; }

    const ClientConfig& config() const noexcept { return cfg_; }
    std::uint64_t requests_ok() const noexcept { return requests_ok_; }
    std::uint64_t requests_failed() const noexcept { return requests_failed_; }
    std::uint64_t reconnects() const noexcept { return reconnects_; }

    // 把 16 位大端载荷解成寄存器数组（供调用方复用）
    static std::vector<std::uint16_t> decode_u16(const std::vector<std::uint8_t>& payload);

private:
    struct Impl;   // asio socket 藏在 .cpp，头文件不依赖 asio

    void set_error(const std::string& what);
    bool ensure_open();

    ClientConfig cfg_;
    void* impl_ = nullptr;   // asio socket 藏在 .cpp，头文件不依赖 asio
    bool open_ = false;
    FrameParser parser_;
    std::string last_error_;
    bool had_exception_ = false;
    ExceptionCode last_exception_ = ExceptionCode::kDeviceFailure;
    std::uint64_t requests_ok_ = 0;
    std::uint64_t requests_failed_ = 0;
    std::uint64_t reconnects_ = 0;
};

}  // namespace gw

#endif  // GW_CLIENT_H
