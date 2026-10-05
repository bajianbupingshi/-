// 帧编解码：结构常量、往返一致性、非法输入必须被编码期拦下
#include <string>
#include <vector>

#include "gtest_shim.h"
#include "gw/crc16.h"
#include "gw/frame.h"
#include "gw/parser.h"

namespace {

std::vector<std::uint16_t> regs(std::initializer_list<std::uint16_t> v) {
    return std::vector<std::uint16_t>(v);
}

}  // namespace

// ── 线路格式 ────────────────────────────────────────────────────────────────
TEST(Frame, 魔数与长度前缀位置正确) {
    const gw::Frame f = gw::make_write_single_request(0x0004, 0x1234);
    const std::vector<std::uint8_t> wire = gw::encode(f);

    EXPECT_EQ(wire[0], std::uint8_t{0x47});
    EXPECT_EQ(wire[1], std::uint8_t{0x57});
    // LEN = 5 + payload_len = 5 + 2 = 7
    EXPECT_EQ(wire[2], std::uint8_t{0x00});
    EXPECT_EQ(wire[3], std::uint8_t{0x00});
    EXPECT_EQ(wire[4], std::uint8_t{0x00});
    EXPECT_EQ(wire[5], std::uint8_t{0x07});
    // 帧总长 = 8 + LEN
    EXPECT_EQ(wire.size(), gw::kFrameOverhead + 7u);
}

TEST(Frame, 地址与数量为大端) {
    const gw::Frame f = gw::make_write_single_request(0x1234, 0xBEEF);
    const std::vector<std::uint8_t> wire = gw::encode(f);
    EXPECT_EQ(wire[7], std::uint8_t{0x12});  // ADDR 高字节
    EXPECT_EQ(wire[8], std::uint8_t{0x34});  // ADDR 低字节
    EXPECT_EQ(wire[9], std::uint8_t{0x00});  // QTY 高字节
    EXPECT_EQ(wire[10], std::uint8_t{0x01}); // QTY 低字节
    EXPECT_EQ(wire[11], std::uint8_t{0xBE}); // 载荷高字节
    EXPECT_EQ(wire[12], std::uint8_t{0xEF}); // 载荷低字节
}

TEST(Frame, CRC低字节在前且覆盖全帧除CRC自身) {
    const gw::Frame f = gw::make_write_single_request(0x0004, 0x1234);
    const std::vector<std::uint8_t> wire = gw::encode(f);
    const std::size_t n = wire.size();
    const std::uint16_t expect = gw::crc16_modbus(wire.data(), n - 2);
    const std::uint16_t stored =
        static_cast<std::uint16_t>(wire[n - 2] | (static_cast<std::uint16_t>(wire[n - 1]) << 8));
    EXPECT_EQ(stored, expect);
    EXPECT_EQ(wire[n - 2], static_cast<std::uint8_t>(expect & 0xFFu));  // 低字节在前
}

// ── 往返一致（表驱动）──────────────────────────────────────────────────────
TEST(Frame, 编解码往返一致_表驱动) {
    std::vector<gw::Frame> frames;
    frames.push_back(gw::make_read_request(0x01, 0x0000, 10));
    frames.push_back(gw::make_read_request(0x02, 0x0010, 64));
    frames.push_back(gw::make_read_response(0x01, 0x0000, regs({1, 2, 3, 4, 5})));
    frames.push_back(gw::make_write_single_request(0x0004, 0x1234));
    frames.push_back(gw::make_write_single_request(0xFFFF, 0x0000));
    {
        gw::Frame f;
        f.func = static_cast<std::uint8_t>(gw::Func::kWriteMulti);
        f.addr = 0x0100;
        f.qty = 3;
        f.payload = {0x00, 0x01, 0x00, 0x02, 0x00, 0x03};
        frames.push_back(f);
    }
    frames.push_back(gw::make_identity_response("NGWP-SIM/1.0"));
    frames.push_back(gw::make_exception(0x01, 0x0000, 10, gw::ExceptionCode::kIllegalAddress));

    for (const gw::Frame& f : frames) {
        const std::vector<std::uint8_t> wire = gw::encode(f);
        gw::FrameParser parser;
        const std::vector<gw::ParseEvent> ev = parser.feed(wire);

        ASSERT_EQ(ev.size(), std::size_t{1});
        const gw::DecodedFrame* decoded = std::get_if<gw::DecodedFrame>(&ev[0]);
        ASSERT_TRUE(decoded != nullptr);

        EXPECT_EQ(decoded->frame.func, f.func);
        EXPECT_EQ(decoded->frame.addr, f.addr);
        EXPECT_EQ(decoded->frame.qty, f.qty);
        EXPECT_EQ(decoded->frame.payload, f.payload);
        EXPECT_EQ(decoded->offset, std::size_t{0});
    }
}

// ── 非法输入必须在编码期被拦下 ──────────────────────────────────────────────
TEST(Frame, 非法帧编码期抛EncodeError) {
    gw::Frame qty_zero = gw::make_read_request(0x01, 0x0000, 0);
    EXPECT_THROW(gw::encode(qty_zero), gw::EncodeError);

    gw::Frame qty_too_big = gw::make_read_request(0x01, 0x0000, 65);
    EXPECT_THROW(gw::encode(qty_too_big), gw::EncodeError);

    gw::Frame payload_mismatch = gw::make_read_request(0x01, 0x0000, 4);
    payload_mismatch.payload = {0x00, 0x01};  // 4 个寄存器却只给了 1 个值
    EXPECT_THROW(gw::encode(payload_mismatch), gw::EncodeError);

    gw::Frame identity_bad_qty = gw::make_identity_response("x");
    identity_bad_qty.qty = 3;  // READ_IDENTITY 要求 QTY == 0
    EXPECT_THROW(gw::encode(identity_bad_qty), gw::EncodeError);

    gw::Frame exc_bad_len = gw::make_exception(0x01, 0, 1, gw::ExceptionCode::kDeviceFailure);
    exc_bad_len.payload.push_back(0x00);  // 异常帧载荷必须恰好 1 字节
    EXPECT_THROW(gw::encode(exc_bad_len), gw::EncodeError);

    gw::Frame unknown;
    unknown.func = 0x7E;  // 未知功能码（未置异常位）
    unknown.qty = 1;
    EXPECT_THROW(gw::encode(unknown), gw::EncodeError);

    gw::Frame identity_huge = gw::make_identity_response(std::string(300, 'A'));
    EXPECT_THROW(gw::encode(identity_huge), gw::EncodeError);
}

// ── 文本化 ──────────────────────────────────────────────────────────────────
TEST(Frame, 十六进制输出格式为大写空格分隔) {
    const std::vector<std::uint8_t> v = {0x47, 0x57, 0x0A, 0xFF};
    EXPECT_EQ(gw::to_hex(v), std::string("47 57 0A FF"));
}

TEST(Frame, 标准向量逐字节钉住_与协议文档保持一致) {
    // 这条断言把 docs/protocol.md §7 的向量与实现绑死：
    // 只要有人改了编码逻辑（或改了文档却没改代码），这里立刻报错。
    const std::vector<std::uint8_t> want_read = {0x47, 0x57, 0x00, 0x00, 0x00, 0x05, 0x01,
                                                 0x00, 0x00, 0x00, 0x0A, 0x46, 0x58};
    EXPECT_EQ(gw::encode(gw::make_read_request(0x01, 0x0000, 10)), want_read);

    const std::vector<std::uint8_t> want_write = {0x47, 0x57, 0x00, 0x00, 0x00, 0x07, 0x03, 0x00,
                                                  0x04, 0x00, 0x01, 0x12, 0x34, 0x1C, 0xF6};
    EXPECT_EQ(gw::encode(gw::make_write_single_request(0x0004, 0x1234)), want_write);

    const std::vector<std::uint8_t> want_resp = {0x47, 0x57, 0x00, 0x00, 0x00, 0x0B, 0x01, 0x00,
                                                 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x02, 0x00,
                                                 0x03, 0x83, 0xBD};
    EXPECT_EQ(gw::encode(gw::make_read_response(0x01, 0x0000, {1, 2, 3})), want_resp);
}

TEST(Frame, 功能码与异常码文本化) {
    EXPECT_EQ(gw::func_name(0x01), std::string("READ_HOLDING(0x01)"));
    EXPECT_EQ(gw::func_name(0x85), std::string("READ_IDENTITY(0x85)[exc]"));
    EXPECT_EQ(gw::exception_name(0x02), std::string("ILLEGAL_ADDRESS"));
}
