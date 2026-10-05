// 设备模型（语义层）：功能码覆盖、异常路径、写后读回、注入联动、进程内确定性
//
// 与 test_parser.cpp 的分工：
//   test_parser  → 形状层（魔数/长度/CRC/结构）
//   test_device  → 语义层（功能码/数量/地址/写入生效）
#include <cstdint>
#include <vector>

#include "gtest_shim.h"
#include "gw/device_model.h"
#include "gw/frame.h"
#include "gw/injection.h"
#include "gw/parser.h"

namespace {

gw::ExceptionCode exc_of(const gw::Frame& f) {
    return static_cast<gw::ExceptionCode>(f.payload.at(0));
}

}  // namespace

// ── 读 ──────────────────────────────────────────────────────────────────────
TEST(DeviceModel, 读保持寄存器返回初始零) {
    gw::DeviceModel dev(16, 4);
    const gw::Frame resp = dev.handle(gw::make_read_request(0x01, 2, 4));
    EXPECT_FALSE(resp.is_exception());
    EXPECT_EQ(resp.addr, std::uint16_t{2});
    EXPECT_EQ(resp.qty, std::uint16_t{4});
    EXPECT_EQ(resp.payload.size(), std::size_t{8});
    for (std::uint8_t b : resp.payload) {
        EXPECT_EQ(b, std::uint8_t{0});
    }
}

TEST(DeviceModel, 读输入寄存器与保持寄存器是两个空间) {
    gw::DeviceModel dev(8, 8);
    dev.table().write(1, 0xBEEF);              // 只写保持寄存器
    const gw::Frame hold = dev.handle(gw::make_read_request(0x01, 1, 1));
    const gw::Frame in = dev.handle(gw::make_read_request(0x02, 1, 1));
    EXPECT_EQ(hold.payload[0], std::uint8_t{0xBE});
    EXPECT_EQ(hold.payload[1], std::uint8_t{0xEF});
    EXPECT_EQ(in.payload[0], std::uint8_t{0x00});  // 输入空间不受影响
    EXPECT_EQ(in.payload[1], std::uint8_t{0x00});
}

TEST(DeviceModel, 读数量非法回ILLEGAL_QUANTITY) {
    gw::DeviceModel dev(16, 0);
    EXPECT_EQ(exc_of(dev.handle(gw::make_read_request(0x01, 0, 0))),
              gw::ExceptionCode::kIllegalQuantity);
    EXPECT_EQ(exc_of(dev.handle(gw::make_read_request(0x01, 0, gw::kMaxQty + 1))),
              gw::ExceptionCode::kIllegalQuantity);
}

TEST(DeviceModel, 读地址越界回ILLEGAL_ADDRESS) {
    gw::DeviceModel dev(16, 0);
    EXPECT_EQ(exc_of(dev.handle(gw::make_read_request(0x01, 16, 1))),
              gw::ExceptionCode::kIllegalAddress);
    EXPECT_EQ(exc_of(dev.handle(gw::make_read_request(0x01, 14, 4))),
              gw::ExceptionCode::kIllegalAddress);  // 尾端越界
    EXPECT_TRUE(dev.handle(gw::make_read_request(0x01, 12, 4)).is_exception() == false);
}

// ── 写单个 ──────────────────────────────────────────────────────────────────
TEST(DeviceModel, 写单个回显且读回一致) {
    gw::DeviceModel dev(16, 0);
    const gw::Frame resp = dev.handle(gw::make_write_single_request(5, 0x1234));
    EXPECT_FALSE(resp.is_exception());
    EXPECT_EQ(resp.func_code(), std::uint8_t{0x03});
    EXPECT_EQ(resp.addr, std::uint16_t{5});
    EXPECT_EQ(resp.payload.size(), std::size_t{2});
    EXPECT_EQ(resp.payload[0], std::uint8_t{0x12});
    EXPECT_EQ(resp.payload[1], std::uint8_t{0x34});
    // 读回
    const gw::Frame back = dev.handle(gw::make_read_request(0x01, 5, 1));
    EXPECT_EQ(back.payload[0], std::uint8_t{0x12});
    EXPECT_EQ(back.payload[1], std::uint8_t{0x34});
    EXPECT_EQ(dev.table().write_count(), std::uint64_t{1});
}

TEST(DeviceModel, 写单个数量非1回ILLEGAL_QUANTITY) {
    gw::DeviceModel dev(16, 0);
    gw::Frame f = gw::make_write_single_request(0, 1);
    f.qty = 2;
    EXPECT_EQ(exc_of(dev.handle(f)), gw::ExceptionCode::kIllegalQuantity);
}

TEST(DeviceModel, 写单个地址越界回ILLEGAL_ADDRESS) {
    gw::DeviceModel dev(4, 0);
    EXPECT_EQ(exc_of(dev.handle(gw::make_write_single_request(4, 1))),
              gw::ExceptionCode::kIllegalAddress);
}

// ── 写多个（docs/protocol.md §3 定义为受支持，必须有实现）────────────────────
TEST(DeviceModel, 写多个回显且读回一致) {
    gw::DeviceModel dev(16, 0);
    gw::Frame req;
    req.func = static_cast<std::uint8_t>(gw::Func::kWriteMulti);
    req.addr = 4;
    req.qty = 3;
    req.payload = {0x00, 0x0A, 0x00, 0x14, 0x00, 0x1E};

    const gw::Frame resp = dev.handle(req);
    EXPECT_FALSE(resp.is_exception());
    EXPECT_EQ(resp.func_code(), std::uint8_t{0x04});
    EXPECT_EQ(resp.payload, req.payload);  // 回显

    const gw::Frame back = dev.handle(gw::make_read_request(0x01, 4, 3));
    EXPECT_EQ(back.payload, req.payload);
    EXPECT_EQ(dev.table().write_count(), std::uint64_t{3});
}

TEST(DeviceModel, 写多个载荷长度与数量不符回ILLEGAL_QUANTITY) {
    gw::DeviceModel dev(16, 0);
    gw::Frame req;
    req.func = static_cast<std::uint8_t>(gw::Func::kWriteMulti);
    req.addr = 0;
    req.qty = 3;
    req.payload = {0x00, 0x0A, 0x00, 0x14};  // 只有 2 个寄存器
    EXPECT_EQ(exc_of(dev.handle(req)), gw::ExceptionCode::kIllegalQuantity);
}

TEST(DeviceModel, 写多个尾端越界回ILLEGAL_ADDRESS) {
    gw::DeviceModel dev(8, 0);
    gw::Frame req;
    req.func = static_cast<std::uint8_t>(gw::Func::kWriteMulti);
    req.addr = 6;
    req.qty = 4;
    req.payload = {0, 1, 0, 2, 0, 3, 0, 4};
    EXPECT_EQ(exc_of(dev.handle(req)), gw::ExceptionCode::kIllegalAddress);
}

// ── 身份与未知功能码 ────────────────────────────────────────────────────────
TEST(DeviceModel, 读身份返回配置的字符串) {
    gw::DeviceModel dev(8, 0, "NGWP-TEST/9.9");
    const gw::Frame req = gw::make_read_request(0x05, 0, 0);
    const gw::Frame resp = dev.handle(req);
    EXPECT_FALSE(resp.is_exception());
    EXPECT_EQ(std::string(resp.payload.begin(), resp.payload.end()), std::string("NGWP-TEST/9.9"));
}

TEST(DeviceModel, 未知功能码回ILLEGAL_FUNCTION而非静默丢弃) {
    gw::DeviceModel dev(8, 0);
    gw::Frame f;
    f.func = 0x7E;
    f.addr = 0;
    f.qty = 1;
    const gw::Frame resp = dev.handle(f);
    EXPECT_TRUE(resp.is_exception());
    EXPECT_EQ(exc_of(resp), gw::ExceptionCode::kIllegalFunction);
}

TEST(DeviceModel, 收到异常帧属于协议误用) {
    gw::DeviceModel dev(8, 0);
    const gw::Frame resp =
        dev.handle(gw::make_exception(0x01, 0, 1, gw::ExceptionCode::kDeviceFailure));
    EXPECT_EQ(exc_of(resp), gw::ExceptionCode::kIllegalFunction);
}

// ── 契约：畸形输入一律不抛异常 ──────────────────────────────────────────────
TEST(DeviceModel, 畸形请求不抛异常且都返回异常帧) {
    gw::DeviceModel dev(16, 4);
    std::vector<gw::Frame> bad;

    {   gw::Frame f; f.func = 0x7E; f.qty = 1; bad.push_back(f); }
    {   gw::Frame f; f.func = 0x03; f.addr = 0; f.qty = 1; f.payload = {1, 2, 3};
        bad.push_back(f); }
    {   gw::Frame f; f.func = 0x04; f.addr = 0; f.qty = 3; f.payload = {1};
        bad.push_back(f); }
    {   gw::Frame f; f.func = 0x01; f.addr = 0; f.qty = 0; bad.push_back(f); }
    {   gw::Frame f; f.func = 0x01; f.addr = 60000; f.qty = 1; bad.push_back(f); }
    {   gw::Frame f; f.func = 0x05; f.addr = 0; f.qty = 9; bad.push_back(f); }
    {   gw::Frame f; f.func = 0x81; f.addr = 0; f.qty = 1; f.payload = {2};
        bad.push_back(f); }
    {   gw::Frame f; f.func = 0x00; f.qty = 0; bad.push_back(f); }

    for (const gw::Frame& f : bad) {
        bool threw = false;
        gw::Frame resp;
        try {
            resp = dev.handle(f);
        } catch (...) {
            threw = true;
        }
        EXPECT_FALSE(threw);                       // 契约：不向调用方抛
        EXPECT_TRUE(resp.is_exception());          // 且必须给明确错误响应
        EXPECT_EQ(resp.payload.size(), std::size_t{1});
    }
}

// ── 注入联动 ────────────────────────────────────────────────────────────────
TEST(DeviceModel, tick写入注入值且tick序号递增) {
    gw::DeviceModel dev(64, 0, gw::kDefaultIdentity, 7);
    dev.set_bindings({gw::RegisterBinding{0, gw::Constant{0x1234}},
                      gw::RegisterBinding{1, gw::Sine{1000.0, 100.0, 100, 0.0}}});
    EXPECT_EQ(dev.tick_index(), std::uint64_t{0});
    dev.tick(0);
    EXPECT_EQ(dev.tick_index(), std::uint64_t{1});
    const gw::Frame r = dev.handle(gw::make_read_request(0x01, 0, 2));
    EXPECT_EQ(r.payload[0], std::uint8_t{0x12});
    EXPECT_EQ(r.payload[1], std::uint8_t{0x34});
    EXPECT_EQ(static_cast<std::uint16_t>((r.payload[2] << 8) | r.payload[3]),
              std::uint16_t{1000});
}

// ── 确定性 ──────────────────────────────────────────────────────────────────
TEST(DeviceModel, 同种子同绑定逐位可重放) {
    auto run = [](std::uint64_t seed) {
        gw::DeviceModel dev(16, 0, gw::kDefaultIdentity, seed);
        dev.set_bindings({gw::RegisterBinding{0, gw::RandomWalk{30000, 64, 0, 65535}},
                          gw::RegisterBinding{1, gw::RandomWalk{5000, 8, 0, 65535}}});
        std::vector<std::uint16_t> trace;
        for (std::uint64_t t = 0; t < 64; ++t) {
            dev.tick(t);
            const gw::Frame r = dev.handle(gw::make_read_request(0x01, 0, 2));
            for (std::uint8_t b : r.payload) {
                trace.push_back(b);
            }
        }
        return trace;
    };
    EXPECT_EQ(run(123), run(123));
    EXPECT_NE(run(123), run(124));
}

// ── 端到端：模型 ↔ 线路格式 ─────────────────────────────────────────────────
TEST(DeviceModel, 响应经编码解析后语义不变) {
    gw::DeviceModel dev(32, 0, "NGWP-E2E/1.0");
    dev.table().write(3, 0x0102);

    const std::vector<gw::Frame> requests = {
        gw::make_read_request(0x01, 3, 1),
        gw::make_read_request(0x02, 0, 4),
        gw::make_write_single_request(7, 0xABCD),
        gw::make_read_request(0x05, 0, 0),
        gw::Frame{},
    };
    for (const gw::Frame& req : requests) {
        const gw::Frame resp = dev.handle(req);
        const std::vector<std::uint8_t> wire = gw::encode(resp);

        gw::FrameParser parser;
        const std::vector<gw::ParseEvent> ev = parser.feed(wire);
        ASSERT_EQ(ev.size(), std::size_t{1});
        const auto* decoded = std::get_if<gw::DecodedFrame>(&ev[0]);
        ASSERT_TRUE(decoded != nullptr);
        EXPECT_EQ(decoded->frame.func, resp.func);
        EXPECT_EQ(decoded->frame.addr, resp.addr);
        EXPECT_EQ(decoded->frame.qty, resp.qty);
        EXPECT_EQ(decoded->frame.payload, resp.payload);
    }
}
