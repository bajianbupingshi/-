#ifndef GW_DEVICE_MODEL_H
#define GW_DEVICE_MODEL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gw/frame.h"
#include "gw/injection.h"
#include "gw/register_table.h"

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// 设备模型：把「请求帧」变成「响应帧」。
//
// 职责边界（与解析器互补）：
//   FrameParser  → 只判**形状**（魔数/长度/CRC/载荷与 QTY 是否自洽）
//   DeviceModel  → 只判**语义**（功能码认不认、数量合不合法、地址越不越界）
// 这样切分的好处：协议畸形在解析层就被拦掉，业务规则在模型层集中可测，
// 两者都能脱离网络单独验证。
//
// 用途（同一个类，三处复用）：
//   1) apps/proto_demo   —— 协议层演示
//   2) apps/sim          —— asio 设备模拟器（W2）
//   3) plugins/driver    —— Neuron 驱动插件的对端语义参考（W2）
// ─────────────────────────────────────────────────────────────────────────────

constexpr const char* kDefaultIdentity = "NGWP-SIM/1.0";

class DeviceModel {
public:
    DeviceModel(std::size_t holding_count,
                std::size_t input_count,
                std::string identity = kDefaultIdentity,
                std::uint64_t seed = 20261005);

    // ── 寄存器 ──────────────────────────────────────────────────────────────
    RegisterTable& table() noexcept { return table_; }
    const RegisterTable& table() const noexcept { return table_; }

    // ── 数据注入 ────────────────────────────────────────────────────────────
    void set_bindings(std::vector<RegisterBinding> bindings) { bindings_ = std::move(bindings); }
    std::vector<RegisterBinding>& bindings() noexcept { return bindings_; }
    const std::vector<RegisterBinding>& bindings() const noexcept { return bindings_; }

    // 推进一个采样周期：按顺序把所有注入策略写入寄存器表。
    // tick 从 0 开始；同一 (seed, bindings, tick 序列) 必须逐位可重放。
    void tick(std::uint64_t tick_index);

    // ── 请求处理 ────────────────────────────────────────────────────────────
    //
    // **保证不向调用方抛异常**：任何内部异常都会被折成一张 kDeviceFailure 异常响应。
    // 这条约束不是洁癖 —— 网络层（asio 回调 / Neuron 的 extern "C" 边界）里
    // 让异常逃逸是未定义行为的温床，见方案 §5②。
    Frame handle(const Frame& request);

    const std::string& identity() const noexcept { return identity_; }
    std::uint64_t tick_index() const noexcept { return tick_index_; }

private:
    RegisterTable table_;
    std::vector<RegisterBinding> bindings_;
    DeterministicRng rng_;
    std::string identity_;
    std::uint64_t tick_index_ = 0;
};

// 大端载荷 → 16 位寄存器数组。载荷长度非偶数时，末字节按高字节补齐（并返回 false）。
std::vector<std::uint16_t> decode_payload_u16(const std::vector<std::uint8_t>& payload,
                                              bool* ok = nullptr);

}  // namespace gw

#endif  // GW_DEVICE_MODEL_H
