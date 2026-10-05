// 协议层演示：把「设备侧 → 线路字节 → 解析器」整条链路跑一遍，并做确定性重放自检。
//
// 这个程序是给实验报告与面试演示用的：输出是可复现的，同种子两次运行逐位一致。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "gw/device_model.h"
#include "gw/frame.h"
#include "gw/injection.h"
#include "gw/parser.h"
#include "gw/register_table.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("    [FAIL] %s\n", what.c_str());
    }
}

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& data) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t b : data) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::size_t count_frames(const std::vector<gw::ParseEvent>& ev) {
    std::size_t n = 0;
    for (const auto& e : ev) {
        if (std::holds_alternative<gw::DecodedFrame>(e)) {
            ++n;
        }
    }
    return n;
}

// 设备侧的请求处理已抽到 common/gw/device_model.h（W2 的 asio 模拟器与
// Neuron 驱动插件共用同一实现）。本演示程序不再自带一份。

// ── 会话：固定种子下产生完全确定的字节流 ────────────────────────────────────
struct SessionResult {
    std::vector<std::uint8_t> stream;
    std::size_t expected_frames = 0;
    std::vector<std::uint16_t> first_tick_values;
};

SessionResult run_session(std::uint64_t seed, std::uint64_t ticks, std::uint16_t point_count) {
    gw::DeviceModel dev(256, 16, gw::kDefaultIdentity, seed);

    // 点位布局（刻意混两种策略，否则 seed 是摆设）：
    //   0 .. point_count-1  ：正弦 —— 无状态，只依赖 tick，**不消耗 RNG**
    //   point_count         ：随机游走 —— 有状态且消耗 RNG，使字节流真正依赖 seed
    // 早期版本只挂了正弦，导致「换 seed 字节流不变」—— 是演示设计的漏洞，不是 RNG 的问题。
    const std::uint16_t walk_addr = point_count;
    auto bindings = gw::make_sine_bindings(0, point_count, gw::Sine{30000.0, 8000.0, 64, 0.0});
    bindings.push_back(gw::RegisterBinding{walk_addr, gw::RandomWalk{30000, 64, 0, 65535}});
    dev.set_bindings(std::move(bindings));

    SessionResult result;
    const std::vector<std::uint16_t> read_addrs = {0, static_cast<std::uint16_t>(point_count / 2),
                                                   walk_addr};

    for (std::uint64_t t = 0; t < ticks; ++t) {
        dev.tick(t);
        if (t == 0) {
            result.first_tick_values = dev.table().read_block(0, point_count, false);
        }
        for (std::uint16_t addr : read_addrs) {
            const gw::Frame req = gw::make_read_request(0x01, addr, 4);
            const gw::Frame resp = dev.handle(req);
            const std::vector<std::uint8_t> w_req = gw::encode(req);
            const std::vector<std::uint8_t> w_resp = gw::encode(resp);
            result.stream.insert(result.stream.end(), w_req.begin(), w_req.end());
            result.stream.insert(result.stream.end(), w_resp.begin(), w_resp.end());
            result.expected_frames += 2;
        }
    }
    return result;
}

void section_vectors() {
    std::printf("[1] 标准帧（可直接复制进 docs/protocol.md）\n");
    const std::vector<std::pair<std::string, gw::Frame>> frames = {
        {"READ_HOLDING req  addr=0x0000 qty=10", gw::make_read_request(0x01, 0x0000, 10)},
        {"WRITE_SINGLE req addr=0x0004 val=0x1234", gw::make_write_single_request(0x0004, 0x1234)},
        {"READ_HOLDING resp addr=0x0000 [1,2,3]", gw::make_read_response(0x01, 0x0000, {1, 2, 3})},
        {"EXCEPTION addr=0x0000 qty=10 code=2", gw::make_exception(0x01, 0x0000, 10, gw::ExceptionCode::kIllegalAddress)},
        {"READ_IDENTITY resp \"NGWP-SIM/1.0\"", gw::make_identity_response("NGWP-SIM/1.0")},
    };
    for (const auto& [label, f] : frames) {
        const std::vector<std::uint8_t> wire = gw::encode(f);
        std::printf("  %-42s (%3zu B)  %s\n", label.c_str(), wire.size(), gw::to_hex(wire).c_str());
    }
}

void section_device(std::uint64_t seed) {
    std::printf("\n[2] 设备侧：寄存器表 + 正弦注入（seed=%llu）\n",
                static_cast<unsigned long long>(seed));
    gw::DeviceModel dev(64, 16, gw::kDefaultIdentity, seed);
    dev.set_bindings(gw::make_sine_bindings(0, 8, gw::Sine{30000.0, 8000.0, 64, 0.0}));

    for (std::uint64_t t = 0; t < 3; ++t) {
        dev.tick(t);
        const std::vector<std::uint16_t> v = dev.table().read_block(0, 8, false);
        std::printf("  tick=%llu  ", static_cast<unsigned long long>(t));
        for (std::uint16_t x : v) {
            std::printf("%6u", x);
        }
        std::printf("\n");
    }

    // 一轮完整的「请求 → 响应」并核对内容
    const gw::Frame req = gw::make_read_request(0x01, 2, 4);
    const gw::Frame resp = dev.handle(req);
    const std::vector<std::uint16_t> expect = dev.table().read_block(2, 4, false);
    const std::vector<std::uint8_t> wire = gw::encode(resp);

    gw::FrameParser parser;
    const std::vector<gw::ParseEvent> ev = parser.feed(wire);
    const auto* decoded = ev.size() == 1 ? std::get_if<gw::DecodedFrame>(&ev[0]) : nullptr;

    const int before = g_checks;
    check(decoded != nullptr, "响应帧应能被解析");
    if (decoded != nullptr) {
        check(decoded->frame.addr == 2, "响应地址应为 2");
        check(decoded->frame.qty == 4, "响应数量应为 4");
        check(decoded->frame.payload.size() == std::size_t{8}, "响应载荷应为 8 字节");
        for (std::size_t i = 0; i < expect.size(); ++i) {
            const std::uint16_t got = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(decoded->frame.payload[i * 2]) << 8) |
                static_cast<std::uint16_t>(decoded->frame.payload[i * 2 + 1]));
            check(got == expect[i], "响应第 " + std::to_string(i) + " 个值应与寄存器表一致");
        }
    }
    std::printf("  请求→响应往返核对完成（%d 项断言）\n", g_checks - before);
}

void section_framing(std::uint64_t seed) {
    std::printf("\n[3] 粘包 / 拆包等价性\n");
    const SessionResult s = run_session(seed, 3, 8);

    gw::FrameParser once;
    const std::vector<gw::ParseEvent> ev_once = once.feed(s.stream);

    gw::FrameParser bytewise;
    std::vector<std::string> seq_bytewise;
    for (std::uint8_t b : s.stream) {
        for (const auto& e : bytewise.feed(&b, 1)) {
            seq_bytewise.push_back(gw::to_string(e));
        }
    }
    std::vector<std::string> seq_once;
    for (const auto& e : ev_once) {
        seq_once.push_back(gw::to_string(e));
    }

    std::printf("  字节流 %zu B：一次性喂入解出 %zu 帧 / %llu 错误；逐字节喂入解出 %llu 帧 / %llu 错误\n",
                s.stream.size(), count_frames(ev_once),
                static_cast<unsigned long long>(once.errors_seen()),
                static_cast<unsigned long long>(bytewise.frames_decoded()),
                static_cast<unsigned long long>(bytewise.errors_seen()));

    check(count_frames(ev_once) == s.expected_frames, "一次性喂入的帧数应与预期一致");
    check(once.errors_seen() == 0, "干净流不应产生解析错误");
    check(seq_bytewise == seq_once, "逐字节喂入的事件序列应与一次性喂入完全一致");
}

void section_resync(std::uint64_t seed) {
    std::printf("\n[4] 坏帧重同步：翻转 1 个 bit 后的行为\n");
    SessionResult s = run_session(seed, 2, 8);
    // 第 1 帧是 READ_HOLDING req addr=0 qty=4，LEN=5，总长 13（偏移 0..12）。
    // 破坏偏移 8（ADDR 低字节，位于 CRC 保护区内且不是魔数）——这样才是「帧内容损坏」，
    // 若破坏偏移 13 那是下一帧的魔数，属于另一种故障（同步丢失），不应混为一谈。
    s.stream[8] ^= 0x01;

    {
        gw::FrameParser p(gw::ResyncPolicy::kSkipFrameOnCrcError);
        const std::vector<gw::ParseEvent> ev = p.feed(s.stream);
        std::printf("  kSkipFrameOnCrcError : 解出 %zu 帧，%llu 个错误\n", count_frames(ev),
                    static_cast<unsigned long long>(p.errors_seen()));
        check(p.errors_seen() == 1, "跳帧策略下应只报 1 个错误");
        check(count_frames(ev) == s.expected_frames - 1, "只应损失被破坏的那 1 帧");
    }
    {
        gw::FrameParser p(gw::ResyncPolicy::kScanByByte);
        const std::vector<gw::ParseEvent> ev = p.feed(s.stream);
        std::printf("  kScanByByte          : 解出 %zu 帧，%llu 个错误\n", count_frames(ev),
                    static_cast<unsigned long long>(p.errors_seen()));
        check(p.errors_seen() > 1, "逐字节扫描策略应产生更多错误事件");
        check(count_frames(ev) == s.expected_frames - 1, "逐字节扫描同样不应漏掉后续帧");
    }
}

void section_replay(std::uint64_t seed) {
    std::printf("\n[5] 确定性重放\n");
    const SessionResult a = run_session(seed, 8, 12);
    const SessionResult b = run_session(seed, 8, 12);
    const SessionResult c = run_session(seed + 1, 8, 12);

    const std::uint64_t ha = fnv1a64(a.stream);
    const std::uint64_t hb = fnv1a64(b.stream);
    const std::uint64_t hc = fnv1a64(c.stream);

    std::printf("  字节流长度        : %zu B\n", a.stream.size());
    std::printf("  FNV-1a(seed=%llu) : %s\n", static_cast<unsigned long long>(seed), hex64(ha).c_str());
    std::printf("  再跑一次          : %s  %s\n", hex64(hb).c_str(), ha == hb ? "(一致)" : "(不一致!)");
    std::printf("  FNV-1a(seed+1)    : %s  %s\n", hex64(hc).c_str(), ha != hc ? "(不同)" : "(竟然相同!)");

    check(a.stream == b.stream, "同种子两次运行的字节流必须逐位一致");
    check(ha != hc, "换种子后字节流必须不同（否则随机源没起作用）");
    check(a.first_tick_values.size() == std::size_t{12}, "首个 tick 的正弦点位数量应为 12");
}

void usage() {
    std::printf("用法: proto_demo [--selftest] [--vectors-only] [--seed N] [--ticks N]\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool selftest = false;
    bool vectors_only = false;
    std::uint64_t seed = 20261005;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") {
            selftest = true;
        } else if (a == "--vectors-only") {
            vectors_only = true;
        } else if (a == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::printf("未知参数: %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    (void)selftest;

    std::printf("=============== NGWP 协议层演示 ===============\n");
    section_vectors();
    if (!vectors_only) {
        section_device(seed);
        section_framing(seed);
        section_resync(seed);
        section_replay(seed);
    }
    std::printf("\nRESULT: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
