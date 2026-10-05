#include "gw/crc16.h"

#include <array>

namespace gw {
namespace {

// 编译期生成 256 项查表。与 crc16_update 的位运算逐位等价（由单测强制互验）。
struct CrcTable {
    std::array<std::uint16_t, 256> v{};

    constexpr CrcTable() {
        for (int i = 0; i < 256; ++i) {
            std::uint16_t crc = static_cast<std::uint16_t>(i);
            for (int bit = 0; bit < 8; ++bit) {
                if ((crc & 0x0001u) != 0u) {
                    crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001u);
                } else {
                    crc = static_cast<std::uint16_t>(crc >> 1);
                }
            }
            v[static_cast<std::size_t>(i)] = crc;
        }
    }
};

constexpr CrcTable kTable{};

}  // namespace

std::uint16_t crc16_modbus_bitwise(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint16_t crc = kCrc16Init;
    for (std::size_t i = 0; i < len; ++i) {
        crc = crc16_update(crc, data[i]);
    }
    return crc;
}

std::uint16_t crc16_modbus(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint16_t crc = kCrc16Init;
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t index = static_cast<std::uint8_t>(crc ^ data[i]);
        crc = static_cast<std::uint16_t>((crc >> 8) ^ kTable.v[index]);
    }
    return crc;
}

}  // namespace gw
