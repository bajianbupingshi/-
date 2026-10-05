#ifndef GW_INJECTION_H
#define GW_INJECTION_H

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "gw/register_table.h"

namespace gw {

// ── 确定性随机源 ────────────────────────────────────────────────────────────
//
// 刻意**不用** <random>：std::mt19937 的分布实现（std::uniform_int_distribution）
// 在不同标准库版本间不保证逐位一致，会让「同种子重放」跨机器失效。
// 这里用 splitmix64，纯整数运算，任何平台结果逐位相同。
class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next_u64() noexcept;
    double next_unit() noexcept;                                       // [0, 1)
    std::int32_t next_inclusive(std::int32_t lo, std::int32_t hi) noexcept;

    std::uint64_t state() const noexcept { return state_; }

private:
    std::uint64_t state_;
};

// ── 注入策略（std::variant + std::visit）────────────────────────────────────
struct Constant {
    std::uint16_t value = 0;
};

struct Sine {
    double mean = 32768.0;
    double amplitude = 8000.0;
    std::uint32_t period_ticks = 100;
    double phase = 0.0;  // 弧度，用于给同一组点位错开相位
};

struct RandomWalk {
    std::int32_t value = 32768;  // 有状态：每次采样后原地更新
    std::int32_t step = 16;      // 最大单步幅度（含）
    std::int32_t lo = 0;
    std::int32_t hi = 65535;
};

using InjectionPolicy = std::variant<Constant, Sine, RandomWalk>;

// 采样一次。policy 非 const —— RandomWalk 需要推进自身状态。
std::uint16_t sample(InjectionPolicy& policy, std::uint64_t tick, DeterministicRng& rng);

// 点位绑定：把策略挂到某个寄存器地址上
struct RegisterBinding {
    std::uint16_t addr = 0;
    InjectionPolicy policy = Constant{};
};

// 一次 tick：把所有绑定按顺序采样并写入寄存器表
void apply_bindings(RegisterTable& table,
                    std::vector<RegisterBinding>& bindings,
                    std::uint64_t tick,
                    DeterministicRng& rng);

// 便捷工厂：给 n 个连续地址挂正弦，相位依次错开（模拟多通道采集）
std::vector<RegisterBinding> make_sine_bindings(std::uint16_t base_addr,
                                                std::uint16_t count,
                                                const Sine& proto);

}  // namespace gw

#endif  // GW_INJECTION_H
