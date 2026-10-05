#include "gw/injection.h"

#include <cmath>
#include <type_traits>

namespace gw {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::int32_t clamp_i32(std::int32_t v, std::int32_t lo, std::int32_t hi) noexcept {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

}  // namespace

std::uint64_t DeterministicRng::next_u64() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double DeterministicRng::next_unit() noexcept {
    // 取高 53 位映到 [0,1)：整数到 double 的转换是精确的，跨平台逐位一致
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

std::int32_t DeterministicRng::next_inclusive(std::int32_t lo, std::int32_t hi) noexcept {
    if (hi <= lo) {
        return lo;
    }
    const std::uint32_t span = static_cast<std::uint32_t>(hi - lo) + 1u;
    // 取模引入的偏差量级为 span/2^64，对注入数据的用途可忽略；换来的是完全确定性
    return lo + static_cast<std::int32_t>(next_u64() % span);
}

std::uint16_t sample(InjectionPolicy& policy, std::uint64_t tick, DeterministicRng& rng) {
    return std::visit(
        [&](auto& p) -> std::uint16_t {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, Constant>) {
                return p.value;
            } else if constexpr (std::is_same_v<T, Sine>) {
                const double period = p.period_ticks == 0u ? 1.0 : static_cast<double>(p.period_ticks);
                const double angle =
                    2.0 * kPi * static_cast<double>(tick) / period + p.phase;
                double v = p.mean + p.amplitude * std::sin(angle);
                if (v < 0.0) {
                    v = 0.0;
                }
                if (v > 65535.0) {
                    v = 65535.0;
                }
                return static_cast<std::uint16_t>(v + 0.5);
            } else {
                const std::int32_t delta = rng.next_inclusive(-p.step, p.step);
                const std::int32_t next = clamp_i32(p.value + delta, p.lo, p.hi);
                p.value = next;
                return static_cast<std::uint16_t>(next);
            }
        },
        policy);
}

void apply_bindings(RegisterTable& table,
                    std::vector<RegisterBinding>& bindings,
                    std::uint64_t tick,
                    DeterministicRng& rng) {
    for (RegisterBinding& b : bindings) {
        table.write(b.addr, sample(b.policy, tick, rng));
    }
}

std::vector<RegisterBinding> make_sine_bindings(std::uint16_t base_addr,
                                                std::uint16_t count,
                                                const Sine& proto) {
    std::vector<RegisterBinding> out;
    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        Sine s = proto;
        s.phase = proto.phase + 2.0 * kPi * static_cast<double>(i) / static_cast<double>(count);
        RegisterBinding b;
        b.addr = static_cast<std::uint16_t>(base_addr + i);
        b.policy = s;
        out.push_back(b);
    }
    return out;
}

}  // namespace gw
