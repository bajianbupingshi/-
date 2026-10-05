// 解析器：粘包 / 拆包 / 重同步 / 长度防护 / 阶段状态机
#include <string>
#include <vector>

#include "gtest_shim.h"
#include "gw/crc16.h"
#include "gw/frame.h"
#include "gw/parser.h"

namespace {

std::vector<std::uint8_t> cat(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// 把事件序列压成可比较的字符串序列
std::vector<std::string> describe(const std::vector<gw::ParseEvent>& ev) {
    std::vector<std::string> out;
    out.reserve(ev.size());
    for (const auto& e : ev) {
        out.push_back(gw::to_string(e));
    }
    return out;
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

std::size_t count_errors(const std::vector<gw::ParseEvent>& ev) {
    return ev.size() - count_frames(ev);
}

gw::ParseErrorCode first_error_code(const std::vector<gw::ParseEvent>& ev) {
    for (const auto& e : ev) {
        if (const auto* err = std::get_if<gw::DecodeError>(&e)) {
            return err->code;
        }
    }
    return gw::ParseErrorCode::kBadMagic;  // 调用方应先确认存在错误
}

}  // namespace

// ── 基本帧 ──────────────────────────────────────────────────────────────────
TEST(Parser, 单帧一次喂入) {
    const std::vector<std::uint8_t> wire =
        gw::encode(gw::make_read_request(0x01, 0x0000, 10));
    gw::FrameParser p;
    const std::vector<gw::ParseEvent> ev = p.feed(wire);

    EXPECT_EQ(ev.size(), std::size_t{1});
    EXPECT_EQ(count_frames(ev), std::size_t{1});
    EXPECT_EQ(p.frames_decoded(), std::uint64_t{1});
    EXPECT_EQ(p.errors_seen(), std::uint64_t{0});
    EXPECT_EQ(p.pending_bytes(), std::size_t{0});
}

// ── 粘包：一次喂入多帧 ──────────────────────────────────────────────────────
TEST(Parser, 粘包_一次喂入三帧应解出三帧且偏移正确) {
    const std::vector<std::uint8_t> f1 = gw::encode(gw::make_read_request(0x01, 0x0000, 10));
    const std::vector<std::uint8_t> f2 = gw::encode(gw::make_write_single_request(0x0004, 0xBEEF));
    const std::vector<std::uint8_t> f3 = gw::encode(gw::make_identity_response("NGWP-SIM/1.0"));

    gw::FrameParser p;
    const std::vector<gw::ParseEvent> ev = p.feed(cat({f1, f2, f3}));

    ASSERT_EQ(ev.size(), std::size_t{3});
    EXPECT_EQ(count_frames(ev), std::size_t{3});

    const auto* d0 = std::get_if<gw::DecodedFrame>(&ev[0]);
    const auto* d1 = std::get_if<gw::DecodedFrame>(&ev[1]);
    const auto* d2 = std::get_if<gw::DecodedFrame>(&ev[2]);
    ASSERT_TRUE(d0 != nullptr && d1 != nullptr && d2 != nullptr);

    EXPECT_EQ(d0->offset, std::size_t{0});
    EXPECT_EQ(d1->offset, f1.size());
    EXPECT_EQ(d2->offset, f1.size() + f2.size());
    EXPECT_EQ(d2->frame.payload.size(), std::string("NGWP-SIM/1.0").size());
}

// ── 拆包等价性：逐字节喂入 == 一次性喂入 ────────────────────────────────────
TEST(Parser, 拆包等价_逐字节喂入事件序列与一次性一致) {
    const std::vector<std::uint8_t> stream = cat({
        gw::encode(gw::make_read_request(0x01, 0x0000, 10)),
        gw::encode(gw::make_read_response(0x01, 0x0000, {11, 22, 33})),
        gw::encode(gw::make_write_single_request(0x0004, 0xBEEF)),
    });

    gw::FrameParser once;
    const std::vector<std::string> expected = describe(once.feed(stream));

    gw::FrameParser byte_by_byte;
    std::vector<std::string> actual;
    for (std::uint8_t b : stream) {
        const std::vector<std::string> part = describe(byte_by_byte.feed(&b, 1));
        actual.insert(actual.end(), part.begin(), part.end());
    }

    EXPECT_EQ(actual, expected);
    EXPECT_EQ(byte_by_byte.frames_decoded(), std::uint64_t{3});
    EXPECT_EQ(byte_by_byte.errors_seen(), std::uint64_t{0});
}

TEST(Parser, 拆包等价_在所有可能切点切成两段结果一致) {
    const std::vector<std::uint8_t> stream = cat({
        gw::encode(gw::make_read_request(0x02, 0x0010, 64)),
        gw::encode(gw::make_identity_response("NGWP-SIM/1.0")),
    });

    gw::FrameParser once;
    const std::vector<std::string> expected = describe(once.feed(stream));

    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        gw::FrameParser p;
        std::vector<std::string> actual = describe(p.feed(stream.data(), cut));
        const std::vector<std::string> tail =
            describe(p.feed(stream.data() + cut, stream.size() - cut));
        actual.insert(actual.end(), tail.begin(), tail.end());
        EXPECT_EQ(actual, expected);
    }
}

// ── 半包：阶段状态机 ────────────────────────────────────────────────────────
TEST(Parser, 半包时phase依次为WaitMagic与WaitLen) {
    gw::FrameParser p;
    EXPECT_EQ(gw::to_string(p.phase()), std::string("WaitMagic(buffered=0)"));

    const std::uint8_t g = 0x47;
    p.feed(&g, 1);
    EXPECT_EQ(gw::to_string(p.phase()), std::string("WaitMagic(buffered=1)"));

    const std::uint8_t w = 0x57;
    p.feed(&w, 1);
    EXPECT_EQ(gw::to_string(p.phase()), std::string("WaitLen(have=0)"));

    const std::uint8_t len_bytes[3] = {0x00, 0x00, 0x00};
    p.feed(len_bytes, 3);
    EXPECT_EQ(gw::to_string(p.phase()), std::string("WaitLen(have=3)"));
}

TEST(Parser, 半包时phase为WaitBody并给出帧总长) {
    const std::vector<std::uint8_t> wire =
        gw::encode(gw::make_read_request(0x01, 0x0000, 10));
    gw::FrameParser p;
    // 只喂固定头：此时已知帧总长，等帧体
    p.feed(wire.data(), gw::kFixedHead);
    EXPECT_EQ(gw::to_string(p.phase()),
              std::string("WaitBody(frame_bytes=") + std::to_string(wire.size()) +
                  ", have=" + std::to_string(gw::kFixedHead) + ")");
    EXPECT_EQ(p.pending_bytes(), gw::kFixedHead);
}

// ── 错误与重同步 ────────────────────────────────────────────────────────────
TEST(Parser, 坏CRC_默认策略只报一个错误并跳过整帧) {
    std::vector<std::uint8_t> bad = gw::encode(gw::make_read_response(0x01, 0x0000, {1, 2, 3}));
    bad[13] ^= 0x01;  // 翻转一个载荷位，CRC 必然失配
    const std::vector<std::uint8_t> good = gw::encode(gw::make_write_single_request(0x0004, 0xBEEF));

    gw::FrameParser p;  // 默认 kSkipFrameOnCrcError
    const std::vector<gw::ParseEvent> ev = p.feed(cat({bad, good}));

    EXPECT_EQ(ev.size(), std::size_t{2});
    EXPECT_EQ(count_errors(ev), std::size_t{1});
    EXPECT_EQ(first_error_code(ev), gw::ParseErrorCode::kBadCrc);
    EXPECT_EQ(count_frames(ev), std::size_t{1});  // 紧随其后的合法帧仍被解出
    EXPECT_EQ(p.frames_decoded(), std::uint64_t{1});
    EXPECT_EQ(p.errors_seen(), std::uint64_t{1});
}

TEST(Parser, 坏CRC_逐字节扫描策略会产生噪声但绝不漏帧) {
    std::vector<std::uint8_t> bad = gw::encode(gw::make_read_response(0x01, 0x0000, {1, 2, 3}));
    bad[13] ^= 0x01;
    const std::vector<std::uint8_t> good = gw::encode(gw::make_write_single_request(0x0004, 0xBEEF));

    gw::FrameParser p(gw::ResyncPolicy::kScanByByte);
    const std::vector<gw::ParseEvent> ev = p.feed(cat({bad, good}));

    EXPECT_EQ(count_frames(ev), std::size_t{1});       // 同样不漏帧
    EXPECT_GT(count_errors(ev), std::size_t{1});       // 但噪声更多
    EXPECT_EQ(p.frames_decoded(), std::uint64_t{1});
}

TEST(Parser, 前置噪声_前导垃圾字节不影响后续帧) {
    const std::vector<std::uint8_t> good = gw::encode(gw::make_read_request(0x01, 0x0000, 1));
    const std::vector<std::uint8_t> noise = {0x00, 0x01, 0x02, 0x47, 0x99, 0xFF};

    gw::FrameParser p;
    const std::vector<gw::ParseEvent> ev = p.feed(cat({noise, good}));

    EXPECT_EQ(count_frames(ev), std::size_t{1});
    // 前 6 个字节逐个都不是魔数起点（第 4 字节是 0x47，但其后是 0x99），故 6 个 BAD_MAGIC
    EXPECT_EQ(p.errors_seen(), std::uint64_t{6});
}

TEST(Parser, 长度超上限_报LENGTH_TOO_LARGE且不分配大缓冲) {
    // 手工构造：合法魔数 + LEN = 0xFFFFFFFF
    const std::vector<std::uint8_t> evil = {0x47, 0x57, 0xFF, 0xFF, 0xFF, 0xFF,
                                            0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    gw::FrameParser p;
    const std::vector<gw::ParseEvent> ev = p.feed(evil);

    EXPECT_GT(count_errors(ev), std::size_t{0});
    EXPECT_EQ(first_error_code(ev), gw::ParseErrorCode::kLengthTooLarge);
    EXPECT_EQ(count_frames(ev), std::size_t{0});
    // 关键：解析器没有因为一个 4 字节字段就申请 4GB
    EXPECT_LE(p.pending_bytes(), evil.size());
}

TEST(Parser, 长度小于下界_同样报LENGTH_TOO_LARGE) {
    const std::vector<std::uint8_t> evil = {0x47, 0x57, 0x00, 0x00, 0x00, 0x04,
                                            0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    gw::FrameParser p;
    const std::vector<gw::ParseEvent> ev = p.feed(evil);
    EXPECT_EQ(first_error_code(ev), gw::ParseErrorCode::kLengthTooLarge);
}

TEST(Parser, 结构不符_载荷长度与QTY矛盾时报BAD_STRUCTURE) {
    // func=READ_HOLDING, qty=4, 但载荷只有 2 字节
    gw::Frame bad;
    bad.func = static_cast<std::uint8_t>(gw::Func::kReadHolding);
    bad.addr = 0x0000;
    bad.qty = 4;
    bad.payload = {0x00, 0x01};

    // 绕过 encode 的结构校验，直接手工拼线（模拟对端发来的畸形帧）
    std::vector<std::uint8_t> wire = {0x47, 0x57, 0x00, 0x00, 0x00, 0x07};
    wire.push_back(bad.func);
    wire.push_back(0x00);
    wire.push_back(0x00);
    wire.push_back(0x00);
    wire.push_back(0x04);
    wire.push_back(0x00);
    wire.push_back(0x01);
    const std::uint16_t crc = gw::crc16_modbus(wire.data(), wire.size());
    wire.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
    wire.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFFu));

    gw::FrameParser p;
    const std::vector<gw::ParseEvent> ev = p.feed(wire);
    EXPECT_EQ(count_frames(ev), std::size_t{0});
    EXPECT_EQ(first_error_code(ev), gw::ParseErrorCode::kBadStructure);
}

TEST(Parser, 未知功能码属于语义问题_解析器不拦) {
    // func=0x7E 未知，但形状合法 → 解析器应放行，由业务层回 ILLEGAL_FUNCTION
    std::vector<std::uint8_t> wire = {0x47, 0x57, 0x00, 0x00, 0x00, 0x06};
    wire.push_back(0x7E);
    wire.push_back(0x00);
    wire.push_back(0x01);
    wire.push_back(0x00);
    wire.push_back(0x01);
    wire.push_back(0x00);
    const std::uint16_t crc = gw::crc16_modbus(wire.data(), wire.size());
    wire.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
    wire.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFFu));

    gw::FrameParser p;
    const std::vector<gw::ParseEvent> ev = p.feed(wire);
    EXPECT_EQ(count_frames(ev), std::size_t{1});
    const auto* d = std::get_if<gw::DecodedFrame>(&ev[0]);
    ASSERT_TRUE(d != nullptr);
    EXPECT_EQ(d->frame.func, std::uint8_t{0x7E});
}

TEST(Parser, 真实流_噪声与多帧混合仍能全部恢复) {
    const std::vector<std::uint8_t> f1 = gw::encode(gw::make_read_request(0x01, 0x0000, 10));
    const std::vector<std::uint8_t> f2 = gw::encode(gw::make_read_response(0x01, 0x0000, {7, 8}));
    const std::vector<std::uint8_t> f3 = gw::encode(gw::make_identity_response("NGWP-SIM/1.0"));
    const std::vector<std::uint8_t> stream =
        cat({{0xAA, 0xBB}, f1, {0x00}, f2, f3, {0x13, 0x37}});

    gw::FrameParser p;
    const std::vector<gw::ParseEvent> ev = p.feed(stream);
    EXPECT_EQ(count_frames(ev), std::size_t{3});
    EXPECT_EQ(p.frames_decoded(), std::uint64_t{3});
}

TEST(Parser, reset后计数与缓冲归零) {
    gw::FrameParser p;
    p.feed(gw::encode(gw::make_read_request(0x01, 0x0000, 1)));
    EXPECT_EQ(p.frames_decoded(), std::uint64_t{1});
    p.reset();
    EXPECT_EQ(p.frames_decoded(), std::uint64_t{0});
    EXPECT_EQ(p.errors_seen(), std::uint64_t{0});
    EXPECT_EQ(p.pending_bytes(), std::size_t{0});
    EXPECT_EQ(gw::to_string(p.phase()), std::string("WaitMagic(buffered=0)"));
}
