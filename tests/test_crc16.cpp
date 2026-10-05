// CRC-16/MODBUS 的三重验证：
//   ① 与标准校验值对齐（外部权威向量，证明算法没跑偏）
//   ② 位运算实现 vs 256 项查表实现逐位一致（两份独立实现互证）
//   ③ 流式逐字节更新与一次性计算等价
#include <cstdint>
#include <string>
#include <vector>

#include "gtest_shim.h"
#include "gw/crc16.h"

namespace {

std::vector<std::uint8_t> bytes_of(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace

// ── ① 外部权威向量 ──────────────────────────────────────────────────────────
TEST(Crc16, 标准校验值_123456789_应得0x4B37) {
    const std::vector<std::uint8_t> data = bytes_of("123456789");
    EXPECT_EQ(gw::crc16_modbus(data.data(), data.size()), std::uint16_t{0x4B37});
    EXPECT_EQ(gw::crc16_modbus_bitwise(data.data(), data.size()), std::uint16_t{0x4B37});
}

TEST(Crc16, 空输入应返回初值) {
    EXPECT_EQ(gw::crc16_modbus(nullptr, 0), gw::kCrc16Init);
    EXPECT_EQ(gw::crc16_modbus_bitwise(nullptr, 0), gw::kCrc16Init);
}

TEST(Crc16, ModbusRTU经典请求帧向量) {
    // 01 03 00 00 00 0A 的 CRC（低字节先传，故报文尾部为 C5 CD）
    const std::vector<std::uint8_t> req = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    EXPECT_EQ(gw::crc16_modbus(req.data(), req.size()), std::uint16_t{0xCDC5});
}

// ── ② 两份独立实现互证（表驱动）──────────────────────────────────────────────
TEST(Crc16, 两实现一致_全256个单字节) {
    for (int v = 0; v < 256; ++v) {
        const std::uint8_t b = static_cast<std::uint8_t>(v);
        EXPECT_EQ(gw::crc16_modbus(&b, 1), gw::crc16_modbus_bitwise(&b, 1));
    }
}

TEST(Crc16, 两实现一致_长度0到300的模式序列) {
    std::vector<std::uint8_t> buf(300);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
    }
    for (std::size_t len = 0; len <= buf.size(); ++len) {
        EXPECT_EQ(gw::crc16_modbus(buf.data(), len),
                  gw::crc16_modbus_bitwise(buf.data(), len));
    }
}

TEST(Crc16, 两实现一致_含0x00与0xFF的极端序列) {
    const std::vector<std::vector<std::uint8_t>> cases = {
        {},
        {0x00},
        {0xFF},
        {0x00, 0x00, 0x00, 0x00},
        {0xFF, 0xFF, 0xFF, 0xFF},
        {0x00, 0xFF, 0x00, 0xFF, 0xAA, 0x55},
        {0x47, 0x57, 0x00, 0x00, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00, 0x01},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(gw::crc16_modbus(c.data(), c.size()),
                  gw::crc16_modbus_bitwise(c.data(), c.size()));
    }
}

// ── ③ 流式等价 ──────────────────────────────────────────────────────────────
TEST(Crc16, 流式逐字节更新等价于一次性计算) {
    const std::vector<std::uint8_t> data = bytes_of("123456789");
    std::uint16_t crc = gw::kCrc16Init;
    for (std::uint8_t b : data) {
        crc = gw::crc16_update(crc, b);
    }
    EXPECT_EQ(crc, gw::crc16_modbus(data.data(), data.size()));
}
