#ifndef GW_GTEST_SHIM_H
#define GW_GTEST_SHIM_H

// ─────────────────────────────────────────────────────────────────────────────
// GoogleTest 兼容垫片
//
// 用途：让**同一份测试源码**既能在 WSL / CI 里链接真正的 GoogleTest，
//       也能在没装 GTest 的开发机（例如本机 Windows + MinGW）上直接编译运行。
//       这样「代码对不对」与「环境装没装」被彻底分开 —— 见技能
//       verify-code-before-target-env-via-api-shim。
//
// 打开方式：CMake 找到系统 GoogleTest 时定义 GW_HAVE_GTEST=1，否则走下面的实现。
// 只实现了本项目用到的子集：TEST / EXPECT_* / ASSERT_* / EXPECT_THROW /
// EXPECT_DOUBLE_EQ（W3-3 起补齐）。与真实框架的已知语义差：ASSERT_* 失败
// 不提前返回（真实框架会 return）—— 已用真实框架全量复跑校验过结果一致。
// ─────────────────────────────────────────────────────────────────────────────

#if defined(GW_HAVE_GTEST)

#include <gtest/gtest.h>

#else  // ── 垫片实现 ──────────────────────────────────────────────────────────

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace gwshim {

struct TestCase {
    const char* suite;
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failure_count() {
    static int n = 0;
    return n;
}

inline int& check_count() {
    static int n = 0;
    return n;
}

inline bool register_test(const char* suite, const char* name, void (*fn)()) {
    registry().push_back(TestCase{suite, name, fn});
    return true;
}

// 失败信息里尽量把值打出来；不可流式输出的类型退化为占位符
template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<
    T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
std::string repr(const T& v) {
    if constexpr (is_streamable<T>::value) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "<unprintable>";
    }
}

inline std::string repr(const std::vector<std::uint8_t>& v) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            s += ' ';
        }
        s += kDigits[(v[i] >> 4) & 0x0Fu];
        s += kDigits[v[i] & 0x0Fu];
    }
    s += ']';
    return s;
}

inline std::string repr(const std::vector<std::uint16_t>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            s += ", ";
        }
        s += std::to_string(v[i]);
    }
    s += ']';
    return s;
}

inline void report_failure(const char* file, int line, const std::string& msg) {
    ++failure_count();
    std::fprintf(stderr, "      %s:%d: %s\n", file, line, msg.c_str());
}

inline int run_all() {
    int failed_tests = 0;
    const int total = static_cast<int>(registry().size());
    for (const TestCase& tc : registry()) {
        const int before = failure_count();
        std::printf("  [ RUN  ] %s.%s\n", tc.suite, tc.name);
        tc.fn();
        const bool ok = failure_count() == before;
        if (!ok) {
            ++failed_tests;
            std::printf("  [ FAIL ] %s.%s\n", tc.suite, tc.name);
        }
    }
    std::printf("\nRESULT: %d tests, %d passed, %d failed (%d assertions)\n", total,
                total - failed_tests, failed_tests, check_count());
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace gwshim

// ── 断言宏（变量名统一加 gw_ 前缀，避免与测试代码里的名字撞 -Wshadow）──
#define GW_CHECK_(cond, text)                                       \
    do {                                                            \
        ++::gwshim::check_count();                                  \
        if (!(cond)) {                                              \
            ::gwshim::report_failure(__FILE__, __LINE__, (text));   \
        }                                                           \
    } while (false)

#define GW_BINOP_(op, a, b, astr, bstr, sym)                                  \
    do {                                                                      \
        ++::gwshim::check_count();                                            \
        const auto gw_a_ = (a);                                               \
        const auto gw_b_ = (b);                                               \
        if (!(gw_a_ op gw_b_)) {                                              \
            ::gwshim::report_failure(                                         \
                __FILE__, __LINE__,                                           \
                std::string(astr " " sym " " bstr) + "  ->  " +               \
                    ::gwshim::repr(gw_a_) + " " sym " " + ::gwshim::repr(gw_b_)); \
        }                                                                     \
    } while (false)

#define EXPECT_TRUE(x)  GW_CHECK_((x), std::string("EXPECT_TRUE(" #x ")"))
#define EXPECT_FALSE(x) GW_CHECK_(!(x), std::string("EXPECT_FALSE(" #x ")"))
#define ASSERT_TRUE(x)  GW_CHECK_((x), std::string("ASSERT_TRUE(" #x ")"))
#define ASSERT_FALSE(x) GW_CHECK_(!(x), std::string("ASSERT_FALSE(" #x ")"))

#define EXPECT_EQ(a, b) GW_BINOP_(==, a, b, #a, #b, "==")
#define EXPECT_NE(a, b) GW_BINOP_(!=, a, b, #a, #b, "!=")
#define EXPECT_LT(a, b) GW_BINOP_(<, a, b, #a, #b, "<")
#define EXPECT_GT(a, b) GW_BINOP_(>, a, b, #a, #b, ">")
#define EXPECT_LE(a, b) GW_BINOP_(<=, a, b, #a, #b, "<=")
#define EXPECT_GE(a, b) GW_BINOP_(>=, a, b, #a, #b, ">=")
#define ASSERT_EQ(a, b) GW_BINOP_(==, a, b, #a, #b, "==")

// 浮点严格相等（4 ULP 近似；与真实 gtest 的 EXPECT_DOUBLE_EQ 对齐，垫片放宽到 eps 缩放）
#define EXPECT_DOUBLE_EQ(a, b)                                                     do {                                                                               ++::gwshim::check_count();                                                     const double gw_a_ = static_cast<double>(a);                                   const double gw_b_ = static_cast<double>(b);                                   const double gw_tol_ =                                                             4.0 * std::numeric_limits<double>::epsilon() *                                 std::max(std::fabs(gw_a_), std::fabs(gw_b_));                              if (!(std::fabs(gw_a_ - gw_b_) <= gw_tol_)) {                                      ::gwshim::report_failure(                                                          __FILE__, __LINE__,                                                            std::string("EXPECT_DOUBLE_EQ(" #a ", " #b ") -> ") +                              std::to_string(gw_a_) + " vs " + std::to_string(gw_b_));           }                                                                          } while (false)

#define EXPECT_NEAR(a, b, eps)                                                 \
    do {                                                                       \
        ++::gwshim::check_count();                                             \
        const double gw_a_ = static_cast<double>(a);                           \
        const double gw_b_ = static_cast<double>(b);                           \
        const double gw_e_ = static_cast<double>(eps);                         \
        if (!(std::abs(gw_a_ - gw_b_) <= gw_e_)) {                             \
            ::gwshim::report_failure(                                          \
                __FILE__, __LINE__,                                            \
                std::string("EXPECT_NEAR(" #a ", " #b ", " #eps ") -> |") +    \
                    std::to_string(gw_a_ - gw_b_) + "| > " +                   \
                    std::to_string(gw_e_));                                    \
        }                                                                      \
    } while (false)

#define EXPECT_THROW(stmt, exc)                                                \
    do {                                                                       \
        ++::gwshim::check_count();                                             \
        bool gw_thrown_ = false;                                               \
        try {                                                                  \
            stmt;                                                              \
        } catch (const exc&) {                                                 \
            gw_thrown_ = true;                                                 \
        } catch (...) {                                                        \
            gw_thrown_ = true;                                                 \
            ::gwshim::report_failure(__FILE__, __LINE__,                       \
                                     "EXPECT_THROW(" #stmt ") 抛出了非预期类型"); \
        }                                                                      \
        if (!gw_thrown_) {                                                     \
            ::gwshim::report_failure(__FILE__, __LINE__,                       \
                                     "EXPECT_THROW(" #stmt ") 未抛异常");       \
        }                                                                      \
    } while (false)

#define TEST(Suite, Name)                                                          \
    static void gwshim_t_##Suite##_##Name();                                       \
    namespace {                                                                    \
    [[maybe_unused]] const bool gwshim_reg_##Suite##_##Name =                      \
        ::gwshim::register_test(#Suite, #Name, &gwshim_t_##Suite##_##Name);        \
    }                                                                              \
    static void gwshim_t_##Suite##_##Name()

#endif  // GW_HAVE_GTEST

#endif  // GW_GTEST_SHIM_H
