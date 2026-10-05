#ifndef GW_FRAME_H
#define GW_FRAME_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace gw {

// ── 帧结构常量（与 docs/protocol.md §2 §4 严格对应）──────────────────────────
constexpr std::uint8_t kMagicByte0 = 0x47u;  // 'G'
constexpr std::uint8_t kMagicByte1 = 0x57u;  // 'W'

constexpr std::size_t kMagicBytes = 2;                        // MAGIC
constexpr std::size_t kLenBytes = 4;                          // LEN
constexpr std::size_t kFixedHead = kMagicBytes + kLenBytes;   // 6：固定头
constexpr std::size_t kFieldBytes = 1 + 2 + 2;                // 5：FUNC + ADDR + QTY
constexpr std::size_t kCrcBytes = 2;                          // CRC16
constexpr std::size_t kFrameOverhead = kFixedHead + kCrcBytes;  // 8：MAGIC+LEN+CRC

constexpr std::uint32_t kMaxPayload = 256;
constexpr std::uint32_t kMaxLen = static_cast<std::uint32_t>(kFieldBytes) + kMaxPayload;  // 261
constexpr std::size_t kMaxFrame = kFrameOverhead + kMaxLen;                              // 269
constexpr std::uint16_t kMaxQty = 64;

// ── 功能码 ───────────────────────────────────────────────────────────────────
enum class Func : std::uint8_t {
    kReadHolding = 0x01,
    kReadInput = 0x02,
    kWriteSingle = 0x03,
    kWriteMulti = 0x04,
    kReadIdentity = 0x05,
};

// FUNC 的最高位是响应/异常标志位
constexpr std::uint8_t kExceptionFlag = 0x80u;

enum class ExceptionCode : std::uint8_t {
    kIllegalFunction = 0x01,
    kIllegalAddress = 0x02,
    kIllegalQuantity = 0x03,
    kDeviceFailure = 0x04,
};

// ── 帧 ──────────────────────────────────────────────────────────────────────
struct Frame {
    std::uint8_t func = 0;  // 含异常标志位
    std::uint16_t addr = 0;
    std::uint16_t qty = 0;  // READ_IDENTITY 固定为 0
    std::vector<std::uint8_t> payload;

    bool is_exception() const noexcept { return (func & kExceptionFlag) != 0u; }
    std::uint8_t func_code() const noexcept {
        return static_cast<std::uint8_t>(func & static_cast<std::uint8_t>(~kExceptionFlag));
    }
};

// 编码期违约束 —— 用异常而非错误码：C++ 层的合法用法是「参数错就是程序员错」。
// 注意：这也正是它**不能穿越 Neuron 的 extern "C" 边界**的原因（见方案 §5②）。
class EncodeError : public std::invalid_argument {
public:
    explicit EncodeError(const std::string& what) : std::invalid_argument(what) {}
};

// 编码为线路字节流；违反 §4 约束时抛 EncodeError
std::vector<std::uint8_t> encode(const Frame& frame);

// 便捷构造
Frame make_read_request(std::uint8_t func, std::uint16_t addr, std::uint16_t qty);
Frame make_read_response(std::uint8_t func, std::uint16_t addr, const std::vector<std::uint16_t>& values);
Frame make_write_single_request(std::uint16_t addr, std::uint16_t value);
Frame make_exception(std::uint8_t func, std::uint16_t addr, std::uint16_t qty, ExceptionCode code);
Frame make_identity_response(const std::string& identity);

// ── 文本化（给日志与测试失败信息用）──────────────────────────────────────────
std::string to_hex(const std::uint8_t* data, std::size_t len);
std::string to_hex(const std::vector<std::uint8_t>& bytes);
std::string func_name(std::uint8_t func);
std::string exception_name(std::uint8_t code);

}  // namespace gw

#endif  // GW_FRAME_H
