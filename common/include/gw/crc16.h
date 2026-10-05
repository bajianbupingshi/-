#ifndef GW_CRC16_H
#define GW_CRC16_H

#include <cstddef>
#include <cstdint>

namespace gw {

// CRC-16/MODBUS 参数：poly 0x8005（反射 0xA001）、init 0xFFFF、refin/refout 均真、xorout 0x0000
constexpr std::uint16_t kCrc16Init = 0xFFFFu;

// CRC-16/MODBUS 对 ASCII "123456789" 的标准校验值（用于自证实现未跑偏）
constexpr std::uint16_t kCrc16CheckValue = 0x4B37u;

// 单字节更新（位运算）。用于流式逐步校验。
inline std::uint16_t crc16_update(std::uint16_t crc, std::uint8_t byte) noexcept {
    crc ^= static_cast<std::uint16_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
        if ((crc & 0x0001u) != 0u) {
            crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001u);
        } else {
            crc = static_cast<std::uint16_t>(crc >> 1);
        }
    }
    return crc;
}

// 参考实现：逐位运算，最直观、最慢
std::uint16_t crc16_modbus_bitwise(const std::uint8_t* data, std::size_t len) noexcept;

// 生产实现：256 项查表
std::uint16_t crc16_modbus(const std::uint8_t* data, std::size_t len) noexcept;

}  // namespace gw

#endif  // GW_CRC16_H
