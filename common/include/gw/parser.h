#ifndef GW_PARSER_H
#define GW_PARSER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "gw/frame.h"

namespace gw {

// ── 解析错误 ────────────────────────────────────────────────────────────────
enum class ParseErrorCode {
    kBadMagic,          // 偏移处不是 0x47 0x57
    kLengthTooLarge,    // LEN < 5 或 LEN > kMaxLen
    kBadCrc,            // CRC 不匹配
    kBadStructure,      // 载荷长度与 QTY/功能码不符（见 protocol.md §4）
};

// ── 解析事件：用 std::variant 表达「成功一帧 / 一个错误」 ─────────────────────
struct DecodedFrame {
    Frame frame;
    std::size_t offset;  // 该帧首字节在本轮输入流中的绝对偏移（自 reset 起累计）
};

struct DecodeError {
    ParseErrorCode code;
    std::size_t offset;
};

using ParseEvent = std::variant<DecodedFrame, DecodeError>;

// ── 解析阶段：显式状态机的状态，同样用 std::variant ──────────────────────────
struct WaitMagic {
    std::size_t buffered;  // 已缓冲但还不足以判定魔数的字节数
};
struct WaitLen {
    std::size_t have;  // 魔数已命中，LEN 字段已收到几个字节（0..3）
};
struct WaitBody {
    std::uint32_t frame_bytes;  // 本帧总字节数 = 8 + LEN
    std::size_t have;           // 已收到几个（恒 < frame_bytes）
};
using Phase = std::variant<WaitMagic, WaitLen, WaitBody>;

// ── 重同步策略 ──────────────────────────────────────────────────────────────
//
// 坏帧出现后「从哪里继续扫」是一个真取舍，没有免费答案，所以做成显式策略：
enum class ResyncPolicy {
    // 逐字节向前找下一个魔数。优点：绝对不会漏掉紧随其后的合法帧；
    // 缺点：一个坏帧会沿路吐出一串 BAD_MAGIC 噪声事件。
    kScanByByte,
    // CRC 失败时直接跳过整帧（帧头已可信，帧长也合法）。优点：一个坏帧只产生 1 个错误事件；
    // 缺点：若帧内的 0x47 0x57 只是载荷巧合，可能连带跳过紧随其后的真帧。
    // 损害有上界：最多跳过 kMaxFrame 字节。
    kSkipFrameOnCrcError,
};

// ── 解析器 ──────────────────────────────────────────────────────────────────
//
// 设计（见 docs/protocol.md §6）：
//  * 内部维护「未消费缓冲 + 游标」，feed() 可以一次吐出多帧（粘包）。
//  * 按 ResyncPolicy 重同步，线路噪声不会连带吃掉紧随其后的合法帧。
//  * 先校验 LEN 上限再分配，畸形报文不能撑爆内存。
//
// 非线程安全：一个连接一个实例（后续在代理层由单线程持有）。
class FrameParser {
public:
    explicit FrameParser(ResyncPolicy policy = ResyncPolicy::kSkipFrameOnCrcError) noexcept
        : policy_(policy) {}

    ResyncPolicy policy() const noexcept { return policy_; }
    void set_policy(ResyncPolicy policy) noexcept { policy_ = policy; }

    // 喂入字节，返回本轮产生的全部事件（按发生顺序）
    std::vector<ParseEvent> feed(const std::uint8_t* data, std::size_t len);
    std::vector<ParseEvent> feed(const std::vector<std::uint8_t>& bytes);

    Phase phase() const;
    std::size_t pending_bytes() const noexcept { return buf_.size() - pos_; }
    std::uint64_t frames_decoded() const noexcept { return frames_; }
    std::uint64_t errors_seen() const noexcept { return errors_; }
    std::uint64_t bytes_consumed() const noexcept { return consumed_; }

    void reset();

private:
    void compact();

    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;      // 已消费到的位置
    ResyncPolicy policy_ = ResyncPolicy::kSkipFrameOnCrcError;
    std::uint64_t consumed_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t errors_ = 0;
};

std::string to_string(ParseErrorCode code);
std::string to_string(const Phase& phase);  // std::visit 分发

// 事件文本化，便于测试断言与日志
std::string to_string(const ParseEvent& event);

}  // namespace gw

#endif  // GW_PARSER_H
