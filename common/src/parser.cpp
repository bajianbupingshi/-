#include "gw/parser.h"

#include <array>
#include <type_traits>

#include "gw/crc16.h"

namespace gw {
namespace {

std::uint16_t read_u16_be(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

std::uint32_t read_u32_be(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

// 结构判据（docs/protocol.md §4）。
// 注意：**未知功能码不判为结构错误** —— 那是语义问题，由业务层回 ILLEGAL_FUNCTION。
// 解析器只负责「这串字节是不是一个形状合法的 NGWP 帧」。
bool structure_ok(const Frame& f, std::size_t payload_len) noexcept {
    if (f.is_exception()) {
        return payload_len == 1u;
    }
    const std::uint8_t code = f.func_code();
    if (code == static_cast<std::uint8_t>(Func::kReadIdentity)) {
        return f.qty == 0u && payload_len <= kMaxPayload;
    }
    const bool is_reg = code >= 0x01u && code <= 0x04u;
    if (is_reg) {
        if (f.qty < 1u || f.qty > kMaxQty) {
            return false;
        }
        return payload_len == 0u || payload_len == std::size_t{2} * f.qty;
    }
    return payload_len <= kMaxPayload;
}

}  // namespace

std::vector<ParseEvent> FrameParser::feed(const std::uint8_t* data, std::size_t len) {
    std::vector<ParseEvent> events;
    if (data != nullptr && len > 0u) {
        buf_.insert(buf_.end(), data, data + len);
    }

    for (;;) {
        const std::size_t avail = buf_.size() - pos_;
        if (avail < kFixedHead) {
            break;  // 固定头都没齐，等下一轮
        }

        const std::uint8_t* p = buf_.data() + pos_;

        if (p[0] != kMagicByte0 || p[1] != kMagicByte1) {
            events.emplace_back(DecodeError{ParseErrorCode::kBadMagic, consumed_});
            ++pos_;
            ++consumed_;
            ++errors_;
            continue;  // 重同步：只丢这 1 个字节
        }

        const std::uint32_t len_field = read_u32_be(p + kMagicBytes);
        if (len_field < kFieldBytes || len_field > kMaxLen) {
            events.emplace_back(DecodeError{ParseErrorCode::kLengthTooLarge, consumed_});
            ++pos_;
            ++consumed_;
            ++errors_;
            continue;
        }

        const std::size_t total = kFrameOverhead + len_field;
        if (avail < total) {
            break;  // 帧体未到齐，等下一轮
        }

        const std::uint16_t stored =
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[total - 2]) |
                                       (static_cast<std::uint16_t>(p[total - 1]) << 8));
        const std::uint16_t computed = crc16_modbus(p, total - kCrcBytes);
        if (stored != computed) {
            events.emplace_back(DecodeError{ParseErrorCode::kBadCrc, consumed_});
            ++errors_;
            if (policy_ == ResyncPolicy::kSkipFrameOnCrcError) {
                // 帧头与帧长都可信，只是内容坏了：整帧跳过，避免沿路喷 BAD_MAGIC
                pos_ += total;
                consumed_ += total;
            } else {
                ++pos_;
                ++consumed_;
            }
            continue;
        }

        Frame f;
        f.func = p[6];
        f.addr = read_u16_be(p + kFixedHead + 1);
        f.qty = read_u16_be(p + kFixedHead + 3);
        const std::size_t payload_len = len_field - kFieldBytes;
        f.payload.assign(p + kFixedHead + kFieldBytes, p + total - kCrcBytes);

        if (!structure_ok(f, payload_len)) {
            events.emplace_back(DecodeError{ParseErrorCode::kBadStructure, consumed_});
            ++pos_;
            ++consumed_;
            ++errors_;
            continue;
        }

        events.emplace_back(DecodedFrame{f, consumed_});
        pos_ += total;
        consumed_ += total;
        ++frames_;
    }

    compact();
    return events;
}

std::vector<ParseEvent> FrameParser::feed(const std::vector<std::uint8_t>& bytes) {
    return feed(bytes.data(), bytes.size());
}

Phase FrameParser::phase() const {
    const std::size_t avail = pending_bytes();
    const std::array<std::uint8_t, 2> magic{kMagicByte0, kMagicByte1};

    if (avail < kFixedHead) {
        std::size_t hit = 0;
        while (hit < magic.size() && hit < avail && buf_[pos_ + hit] == magic[hit]) {
            ++hit;
        }
        if (hit < magic.size()) {
            return WaitMagic{avail};
        }
        return WaitLen{avail - kMagicBytes};  // LEN 已收到 (avail-2) 个字节
    }

    const std::uint32_t len_field = read_u32_be(buf_.data() + pos_ + kMagicBytes);
    const std::size_t total = kFrameOverhead + len_field;
    return WaitBody{static_cast<std::uint32_t>(total), avail};
}

void FrameParser::reset() {
    buf_.clear();
    pos_ = 0;
    consumed_ = 0;
    frames_ = 0;
    errors_ = 0;
}

void FrameParser::compact() {
    if (pos_ == 0) {
        return;
    }
    if (pos_ >= buf_.size()) {
        buf_.clear();
        pos_ = 0;
        return;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ = 0;
}

std::string to_string(ParseErrorCode code) {
    switch (code) {
        case ParseErrorCode::kBadMagic:        return "BAD_MAGIC";
        case ParseErrorCode::kLengthTooLarge:  return "LENGTH_TOO_LARGE";
        case ParseErrorCode::kBadCrc:          return "BAD_CRC";
        case ParseErrorCode::kBadStructure:    return "BAD_STRUCTURE";
    }
    return "UNKNOWN_ERROR";
}

std::string to_string(const Phase& phase) {
    return std::visit(
        [](const auto& s) -> std::string {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, WaitMagic>) {
                return "WaitMagic(buffered=" + std::to_string(s.buffered) + ")";
            } else if constexpr (std::is_same_v<T, WaitLen>) {
                return "WaitLen(have=" + std::to_string(s.have) + ")";
            } else {
                return "WaitBody(frame_bytes=" + std::to_string(s.frame_bytes) +
                       ", have=" + std::to_string(s.have) + ")";
            }
        },
        phase);
}

std::string to_string(const ParseEvent& event) {
    return std::visit(
        [](const auto& e) -> std::string {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, DecodedFrame>) {
                return "FRAME@" + std::to_string(e.offset) + " " +
                       func_name(e.frame.func) + " addr=" + std::to_string(e.frame.addr) +
                       " qty=" + std::to_string(e.frame.qty) +
                       " payload=" + to_hex(e.frame.payload);
            } else {
                return "ERROR@" + std::to_string(e.offset) + " " + to_string(e.code);
            }
        },
        event);
}

}  // namespace gw
