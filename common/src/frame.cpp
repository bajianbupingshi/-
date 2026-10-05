#include "gw/frame.h"

#include <cstdio>

#include "gw/crc16.h"

namespace gw {
namespace {

bool is_register_func(std::uint8_t code) noexcept {
    return code == static_cast<std::uint8_t>(Func::kReadHolding) ||
           code == static_cast<std::uint8_t>(Func::kReadInput) ||
           code == static_cast<std::uint8_t>(Func::kWriteSingle) ||
           code == static_cast<std::uint8_t>(Func::kWriteMulti);
}

void push_u16_be(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

}  // namespace

std::vector<std::uint8_t> encode(const Frame& frame) {
    // ── 结构校验：违反即抛，异常不得穿越 extern "C" 边界（见方案 §5②）──
    if (frame.is_exception()) {
        if (frame.payload.size() != 1u) {
            throw EncodeError("NGWP: 异常帧载荷必须恰好 1 字节");
        }
    } else if (frame.func_code() == static_cast<std::uint8_t>(Func::kReadIdentity)) {
        if (frame.qty != 0u) {
            throw EncodeError("NGWP: READ_IDENTITY 要求 QTY == 0");
        }
        if (frame.payload.size() > kMaxPayload) {
            throw EncodeError("NGWP: READ_IDENTITY 载荷超过 256 字节");
        }
    } else if (is_register_func(frame.func_code())) {
        if (frame.qty < 1u || frame.qty > kMaxQty) {
            throw EncodeError("NGWP: QTY 必须落在 [1, 64]");
        }
        if (!frame.payload.empty() && frame.payload.size() != std::size_t{2} * frame.qty) {
            throw EncodeError("NGWP: 载荷长度必须为 0 或 2*QTY");
        }
    } else {
        throw EncodeError("NGWP: 未知功能码");
    }

    const std::uint32_t len = static_cast<std::uint32_t>(kFieldBytes) +
                              static_cast<std::uint32_t>(frame.payload.size());
    if (len > kMaxLen) {
        throw EncodeError("NGWP: LEN 超过上限 261");
    }

    std::vector<std::uint8_t> out;
    out.reserve(kFrameOverhead + len);
    out.push_back(kMagicByte0);
    out.push_back(kMagicByte1);
    out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(len & 0xFFu));
    out.push_back(frame.func);
    push_u16_be(out, frame.addr);
    push_u16_be(out, frame.qty);
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());

    const std::uint16_t crc = crc16_modbus(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(crc & 0xFFu));         // 低字节在前
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFFu));
    return out;
}

Frame make_read_request(std::uint8_t func, std::uint16_t addr, std::uint16_t qty) {
    Frame f;
    f.func = func;
    f.addr = addr;
    f.qty = qty;
    return f;
}

Frame make_read_response(std::uint8_t func, std::uint16_t addr,
                         const std::vector<std::uint16_t>& values) {
    Frame f;
    f.func = func;
    f.addr = addr;
    f.qty = static_cast<std::uint16_t>(values.size());
    f.payload.reserve(values.size() * 2);
    for (std::uint16_t v : values) {
        push_u16_be(f.payload, v);
    }
    return f;
}

Frame make_write_single_request(std::uint16_t addr, std::uint16_t value) {
    Frame f;
    f.func = static_cast<std::uint8_t>(Func::kWriteSingle);
    f.addr = addr;
    f.qty = 1;
    push_u16_be(f.payload, value);
    return f;
}

Frame make_exception(std::uint8_t func, std::uint16_t addr, std::uint16_t qty,
                     ExceptionCode code) {
    Frame f;
    f.func = static_cast<std::uint8_t>(func | kExceptionFlag);
    f.addr = addr;
    f.qty = qty;
    f.payload.push_back(static_cast<std::uint8_t>(code));
    return f;
}

Frame make_identity_response(const std::string& identity) {
    Frame f;
    f.func = static_cast<std::uint8_t>(Func::kReadIdentity);
    f.addr = 0;
    f.qty = 0;
    f.payload.assign(identity.begin(), identity.end());
    return f;
}

std::string to_hex(const std::uint8_t* data, std::size_t len) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 3);
    for (std::size_t i = 0; i < len; ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out.push_back(kDigits[(data[i] >> 4) & 0x0Fu]);
        out.push_back(kDigits[data[i] & 0x0Fu]);
    }
    return out;
}

std::string to_hex(const std::vector<std::uint8_t>& bytes) {
    return to_hex(bytes.data(), bytes.size());
}

std::string func_name(std::uint8_t func) {
    const bool exc = (func & kExceptionFlag) != 0u;
    const std::uint8_t code = static_cast<std::uint8_t>(func & ~kExceptionFlag);
    const char* base = "UNKNOWN";
    switch (static_cast<Func>(code)) {
        case Func::kReadHolding:  base = "READ_HOLDING";  break;
        case Func::kReadInput:    base = "READ_INPUT";    break;
        case Func::kWriteSingle:  base = "WRITE_SINGLE";  break;
        case Func::kWriteMulti:   base = "WRITE_MULTI";   break;
        case Func::kReadIdentity: base = "READ_IDENTITY"; break;
    }
    std::string out = base;
    out += "(";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned>(func));
    out += buf;
    out += ")";
    if (exc) {
        out += "[exc]";
    }
    return out;
}

std::string exception_name(std::uint8_t code) {
    switch (static_cast<ExceptionCode>(code)) {
        case ExceptionCode::kIllegalFunction: return "ILLEGAL_FUNCTION";
        case ExceptionCode::kIllegalAddress:  return "ILLEGAL_ADDRESS";
        case ExceptionCode::kIllegalQuantity: return "ILLEGAL_QUANTITY";
        case ExceptionCode::kDeviceFailure:   return "DEVICE_FAILURE";
    }
    return "UNKNOWN_EXCEPTION";
}

}  // namespace gw
