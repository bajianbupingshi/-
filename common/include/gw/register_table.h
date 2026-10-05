#ifndef GW_REGISTER_TABLE_H
#define GW_REGISTER_TABLE_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gw {

// 设备侧的寄存器映像：保持寄存器（可读写）+ 输入寄存器（只读）。
// 后续由 asio 设备模拟器持有，Neuron 驱动插件通过协议读写它。
class RegisterTable {
public:
    RegisterTable(std::size_t holding_count, std::size_t input_count = 0);

    std::size_t holding_count() const noexcept { return holding_.size(); }
    std::size_t input_count() const noexcept { return input_.size(); }

    // 区间合法性（含 0 长度拒绝：QTY 由协议层保证 >= 1）
    bool in_range(std::uint16_t addr, std::uint16_t qty, bool input) const noexcept;

    // 越界抛 std::out_of_range —— 调用方在协议层已做 in_range 检查，
    // 这里再抛是「防御性编程的第二道闸」，避免越界静默返回 0。
    std::uint16_t read(std::uint16_t addr, bool input) const;
    std::vector<std::uint16_t> read_block(std::uint16_t addr, std::uint16_t qty, bool input) const;

    // 写保持寄存器。
    // 注意：**不存在写输入寄存器的 API** —— 输入寄存器是只读空间，
    // 协议层靠功能码区分空间（0x01/0x02 读，0x03/0x04 写保持寄存器），
    // 所以「同一地址在两个空间都存在」不会产生歧义。这里只校验保持空间边界。
    void write(std::uint16_t addr, std::uint16_t value);
    void write_block(std::uint16_t addr, const std::vector<std::uint16_t>& values);

    std::uint64_t write_count() const noexcept { return writes_; }
    std::uint64_t read_count() const noexcept { return reads_; }

private:
    std::vector<std::uint16_t> holding_;
    std::vector<std::uint16_t> input_;
    std::uint64_t writes_ = 0;
    mutable std::uint64_t reads_ = 0;
};

}  // namespace gw

#endif  // GW_REGISTER_TABLE_H
