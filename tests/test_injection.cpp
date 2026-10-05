// 注入策略：策略语义 + 确定性重放
//
// 注意确定性测试的分层，别混：
//  * 整数路径（Constant / RandomWalk）要求**逐位可重放**，用严格相等。
//  * Sine 走 libm 的 std::sin，不同平台可能有末位差异，用容差断言，不假装逐位一致。
#include <cmath>
#include <cstdint>
#include <vector>

#include "gtest_shim.h"
#include "gw/injection.h"
#include "gw/register_table.h"

// ── 确定性随机源 ────────────────────────────────────────────────────────────
TEST(DeterministicRng, 同种子产生相同序列) {
    gw::DeterministicRng a(12345), b(12345);
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(a.next_u64(), b.next_u64());
    }
}

TEST(DeterministicRng, 不同种子产生不同序列) {
    gw::DeterministicRng a(1), b(2);
    std::size_t same = 0;
    for (int i = 0; i < 64; ++i) {
        if (a.next_u64() == b.next_u64()) {
            ++same;
        }
    }
    EXPECT_EQ(same, std::size_t{0});
}

TEST(DeterministicRng, 单位区间落在0到1之间) {
    gw::DeterministicRng r(7);
    for (int i = 0; i < 1000; ++i) {
        const double u = r.next_unit();
        EXPECT_GE(u, 0.0);
        EXPECT_LT(u, 1.0);
    }
}

TEST(DeterministicRng, 有界整数不越界且能覆盖两端) {
    gw::DeterministicRng r(99);
    bool hit_lo = false;
    bool hit_hi = false;
    for (int i = 0; i < 4000; ++i) {
        const std::int32_t v = r.next_inclusive(-3, 3);
        EXPECT_GE(v, -3);
        EXPECT_LE(v, 3);
        if (v == -3) {
            hit_lo = true;
        }
        if (v == 3) {
            hit_hi = true;
        }
    }
    EXPECT_TRUE(hit_lo);
    EXPECT_TRUE(hit_hi);
}

// ── 策略语义 ────────────────────────────────────────────────────────────────
TEST(Injection, 常量策略恒定) {
    gw::DeterministicRng r(1);
    gw::InjectionPolicy p = gw::Constant{4242};
    for (std::uint64_t t = 0; t < 10; ++t) {
        EXPECT_EQ(gw::sample(p, t, r), std::uint16_t{4242});
    }
}

TEST(Injection, 正弦策略在幅值区间内且均值附近对称) {
    gw::DeterministicRng r(1);
    gw::InjectionPolicy p = gw::Sine{1000.0, 100.0, 8, 0.0};
    std::int64_t sum = 0;
    const int n = 8;
    for (std::uint64_t t = 0; t < static_cast<std::uint64_t>(n); ++t) {
        const std::uint16_t v = gw::sample(p, t, r);
        EXPECT_GE(v, std::uint16_t{899});   // mean - amp = 900，留 1 个量化余量
        EXPECT_LE(v, std::uint16_t{1101});
        sum += v;
    }
    const double avg = static_cast<double>(sum) / n;
    EXPECT_NEAR(avg, 1000.0, 0.5);
}

TEST(Injection, 正弦策略首点等于均值加相位偏移) {
    gw::DeterministicRng r(1);
    gw::InjectionPolicy p = gw::Sine{1000.0, 100.0, 100, 0.0};
    // tick=0 且 phase=0 时 sin=0，应恰好落在均值上
    EXPECT_EQ(gw::sample(p, 0, r), std::uint16_t{1000});
}

TEST(Injection, 正弦策略输出被夹在0到65535) {
    gw::DeterministicRng r(1);
    gw::InjectionPolicy low = gw::Sine{0.0, 50000.0, 8, 0.0};      // 理论区间 [-50000, 50000]
    gw::InjectionPolicy high = gw::Sine{65535.0, 50000.0, 8, 0.0}; // 理论区间 [15535, 115535]
    for (std::uint64_t t = 0; t < 16; ++t) {
        const std::uint16_t v_low = gw::sample(low, t, r);
        const std::uint16_t v_high = gw::sample(high, t, r);
        EXPECT_LE(v_low, std::uint16_t{50000});    // 上夹紧：不超过 mean+amp
        EXPECT_GE(v_high, std::uint16_t{15535});   // 下夹紧：不低于 mean-amp
    }
}

TEST(Injection, 随机游走始终落在上下界内) {
    gw::DeterministicRng r(2026);
    gw::InjectionPolicy p = gw::RandomWalk{1000, 100, 900, 1100};
    for (int i = 0; i < 5000; ++i) {
        const std::uint16_t v = gw::sample(p, static_cast<std::uint64_t>(i), r);
        EXPECT_GE(v, std::uint16_t{900});
        EXPECT_LE(v, std::uint16_t{1100});
    }
}

TEST(Injection, 随机游走确实在动而不是恒定) {
    gw::DeterministicRng r(2026);
    gw::InjectionPolicy p = gw::RandomWalk{1000, 100, 0, 65535};
    bool moved = false;
    const std::uint16_t first = gw::sample(p, 0, r);
    for (int i = 1; i < 50; ++i) {
        if (gw::sample(p, static_cast<std::uint64_t>(i), r) != first) {
            moved = true;
            break;
        }
    }
    EXPECT_TRUE(moved);
}

// ── 确定性重放（整数路径要求逐位一致）──────────────────────────────────────
TEST(Injection, 同种子两次运行产生完全相同的整数序列) {
    auto run = [](std::uint64_t seed) {
        gw::DeterministicRng rng(seed);
        std::vector<gw::RegisterBinding> bindings;
        bindings.push_back(gw::RegisterBinding{0, gw::Constant{1234}});
        bindings.push_back(gw::RegisterBinding{1, gw::RandomWalk{30000, 64, 0, 65535}});
        bindings.push_back(gw::RegisterBinding{2, gw::RandomWalk{5000, 8, 0, 65535}});
        std::vector<std::uint16_t> trace;
        for (std::uint64_t t = 0; t < 256; ++t) {
            for (auto& b : bindings) {
                trace.push_back(gw::sample(b.policy, t, rng));
            }
        }
        return trace;
    };

    EXPECT_EQ(run(20261005), run(20261005));
}

TEST(Injection, 换种子后整数序列必须不同) {
    auto run = [](std::uint64_t seed) {
        gw::DeterministicRng rng(seed);
        gw::RegisterBinding b{0, gw::RandomWalk{30000, 64, 0, 65535}};
        std::vector<std::uint16_t> trace;
        for (std::uint64_t t = 0; t < 256; ++t) {
            trace.push_back(gw::sample(b.policy, t, rng));
        }
        return trace;
    };
    EXPECT_NE(run(1), run(2));
}

// ── 与寄存器表联动 ──────────────────────────────────────────────────────────
TEST(Injection, 应用到寄存器表) {
    gw::RegisterTable table(16);
    gw::DeterministicRng rng(7);
    auto bindings = gw::make_sine_bindings(4, 4, gw::Sine{1000.0, 10.0, 100, 0.0});

    gw::apply_bindings(table, bindings, 0, rng);
    // tick=0、phase=2πi/4 时，sin 分别为 0, 1, 0, -1
    EXPECT_EQ(table.read(4, false), std::uint16_t{1000});
    EXPECT_EQ(table.read(5, false), std::uint16_t{1010});
    EXPECT_EQ(table.read(6, false), std::uint16_t{1000});
    EXPECT_EQ(table.read(7, false), std::uint16_t{990});
    EXPECT_EQ(table.write_count(), std::uint64_t{4});
}

TEST(Injection, 正弦绑定数量与相位错开) {
    const auto bindings = gw::make_sine_bindings(0, 8, gw::Sine{1000.0, 10.0, 100, 0.0});
    ASSERT_EQ(bindings.size(), std::size_t{8});
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        EXPECT_EQ(bindings[i].addr, static_cast<std::uint16_t>(i));
        const auto* s = std::get_if<gw::Sine>(&bindings[i].policy);
        ASSERT_TRUE(s != nullptr);
        const double expected = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / 8.0;
        EXPECT_NEAR(s->phase, expected, 1e-9);
    }
}
