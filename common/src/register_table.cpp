#include "gw/register_table.h"

namespace gw {

RegisterTable::RegisterTable(std::size_t holding_count, std::size_t input_count)
    : holding_(holding_count, 0u), input_(input_count, 0u) {}

bool RegisterTable::in_range(std::uint16_t addr, std::uint16_t qty, bool input) const noexcept {
    if (qty == 0u) {
        return false;  // 0 长度的区间无意义，直接拒绝
    }
    const std::size_t size = input ? input_.size() : holding_.size();
    const std::size_t start = addr;
    const std::size_t need = qty;
    if (start >= size) {
        return false;
    }
    return need <= size - start;  // 用减法避免 start+need 溢出
}

std::uint16_t RegisterTable::read(std::uint16_t addr, bool input) const {
    const std::vector<std::uint16_t>& src = input ? input_ : holding_;
    if (static_cast<std::size_t>(addr) >= src.size()) {
        throw std::out_of_range("RegisterTable::read 地址越界");
    }
    ++reads_;
    return src[addr];
}

std::vector<std::uint16_t> RegisterTable::read_block(std::uint16_t addr, std::uint16_t qty,
                                                     bool input) const {
    if (!in_range(addr, qty, input)) {
        throw std::out_of_range("RegisterTable::read_block 区间越界");
    }
    const std::vector<std::uint16_t>& src = input ? input_ : holding_;
    reads_ += qty;
    return std::vector<std::uint16_t>(src.begin() + addr, src.begin() + addr + qty);
}

void RegisterTable::write(std::uint16_t addr, std::uint16_t value) {
    if (static_cast<std::size_t>(addr) >= holding_.size()) {
        throw std::out_of_range("RegisterTable::write 地址越界（或试图写只读的输入寄存器）");
    }
    holding_[addr] = value;
    ++writes_;
}

void RegisterTable::write_block(std::uint16_t addr, const std::vector<std::uint16_t>& values) {
    if (values.empty() || values.size() > 65535u) {
        throw std::out_of_range("RegisterTable::write_block 数量非法");
    }
    const auto qty = static_cast<std::uint16_t>(values.size());
    if (!in_range(addr, qty, false)) {
        throw std::out_of_range("RegisterTable::write_block 区间越界");
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        holding_[static_cast<std::size_t>(addr) + i] = values[i];
    }
    writes_ += values.size();
}

}  // namespace gw
