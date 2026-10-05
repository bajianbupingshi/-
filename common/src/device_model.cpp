#include "gw/device_model.h"

namespace gw {

DeviceModel::DeviceModel(std::size_t holding_count, std::size_t input_count, std::string identity,
                         std::uint64_t seed)
    : table_(holding_count, input_count),
      rng_(seed),
      identity_(std::move(identity)) {}

void DeviceModel::tick(std::uint64_t tick_index) {
    apply_bindings(table_, bindings_, tick_index, rng_);
    tick_index_ = tick_index + 1;
}

std::vector<std::uint16_t> decode_payload_u16(const std::vector<std::uint8_t>& payload, bool* ok) {
    std::vector<std::uint16_t> out;
    out.reserve(payload.size() / 2);
    const std::size_t pairs = payload.size() / 2;
    for (std::size_t i = 0; i < pairs; ++i) {
        out.push_back(static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(payload[i * 2]) << 8) |
            static_cast<std::uint16_t>(payload[i * 2 + 1])));
    }
    if (ok != nullptr) {
        *ok = (payload.size() % 2) == 0;
    }
    return out;
}

Frame DeviceModel::handle(const Frame& request) {
    const std::uint8_t code = request.func_code();
    try {
        // 收到「响应/异常帧」本身就是协议误用（设备只该收请求）
        if (request.is_exception()) {
            return make_exception(code, request.addr, request.qty,
                                  ExceptionCode::kIllegalFunction);
        }

        switch (static_cast<Func>(code)) {
            case Func::kReadHolding:
            case Func::kReadInput: {
                const bool input = (code == static_cast<std::uint8_t>(Func::kReadInput));
                if (request.qty < 1u || request.qty > kMaxQty) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalQuantity);
                }
                if (!table_.in_range(request.addr, request.qty, input)) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalAddress);
                }
                return make_read_response(code, request.addr,
                                          table_.read_block(request.addr, request.qty, input));
            }

            case Func::kWriteSingle: {
                if (request.qty != 1u || request.payload.size() != 2u) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalQuantity);
                }
                bool ok = false;
                const std::vector<std::uint16_t> values = decode_payload_u16(request.payload, &ok);
                if (!ok || values.size() != 1u) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalQuantity);
                }
                if (!table_.in_range(request.addr, 1, false)) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalAddress);
                }
                table_.write(request.addr, values[0]);
                return make_read_response(code, request.addr, {values[0]});  // 回显
            }

            case Func::kWriteMulti: {
                // docs/protocol.md §3 把 WRITE_MULTI 定义为受支持功能码，
                // 所以这里必须真的实现它（早期版本直接回 ILLEGAL_FUNCTION，是文档与实现不一致）
                if (request.qty < 1u || request.qty > kMaxQty) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalQuantity);
                }
                bool ok = false;
                const std::vector<std::uint16_t> values = decode_payload_u16(request.payload, &ok);
                if (!ok || values.size() != request.qty) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalQuantity);
                }
                if (!table_.in_range(request.addr, request.qty, false)) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalAddress);
                }
                table_.write_block(request.addr, values);
                return make_read_response(code, request.addr, values);  // 回显
            }

            case Func::kReadIdentity:
                // protocol.md §4 规则 3 要求 READ_IDENTITY 的 QTY == 0。
                // 解析器已经拦过这道，但模型**不能假设调用方一定来自解析器**
                // （W2 的模拟器与驱动插件都可能直接调用 handle()）—— 这是第二道闸。
                if (request.qty != 0u) {
                    return make_exception(code, request.addr, request.qty,
                                          ExceptionCode::kIllegalQuantity);
                }
                return make_identity_response(identity_);
        }

        // 未知功能码：形状没问题，但语义不认 —— 回 ILLEGAL_FUNCTION 而不是静默丢包
        return make_exception(code, request.addr, request.qty, ExceptionCode::kIllegalFunction);
    } catch (...) {
        // 任何内部异常一律折成设备故障响应，绝不向网络层抛
        return make_exception(code, request.addr, request.qty, ExceptionCode::kDeviceFailure);
    }
}

}  // namespace gw
