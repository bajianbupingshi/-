// 寄存器表：区间边界、越界防护、只读语义
#include <vector>

#include "gtest_shim.h"
#include "gw/register_table.h"

TEST(RegisterTable, 初始全零且尺寸正确) {
    const gw::RegisterTable t(16, 4);
    EXPECT_EQ(t.holding_count(), std::size_t{16});
    EXPECT_EQ(t.input_count(), std::size_t{4});
    EXPECT_EQ(t.read(0, false), std::uint16_t{0});
    EXPECT_EQ(t.read(15, false), std::uint16_t{0});
}

TEST(RegisterTable, 读写往返) {
    gw::RegisterTable t(16);
    t.write(3, 0xBEEF);
    EXPECT_EQ(t.read(3, false), std::uint16_t{0xBEEF});
    EXPECT_EQ(t.write_count(), std::uint64_t{1});
}

TEST(RegisterTable, 区间边界_合法与非法) {
    const gw::RegisterTable t(10, 0);
    EXPECT_TRUE(t.in_range(0, 10, false));   // 恰好覆盖全表
    EXPECT_TRUE(t.in_range(9, 1, false));    // 最后一格
    EXPECT_FALSE(t.in_range(10, 1, false));  // 起点越界
    EXPECT_FALSE(t.in_range(5, 6, false));   // 终点越界
    EXPECT_FALSE(t.in_range(0, 0, false));   // 零长度无意义
    EXPECT_FALSE(t.in_range(0, 11, false));  // 超过整表
    EXPECT_FALSE(t.in_range(0, 1, true));    // 没有输入寄存器
}

TEST(RegisterTable, 区间边界_不接受会溢出的极大数量) {
    const gw::RegisterTable t(65535);
    EXPECT_FALSE(t.in_range(60000, 60000, false));  // 若用 start+need 判断会溢出
    EXPECT_TRUE(t.in_range(60000, 5535, false));
}

TEST(RegisterTable, 越界读写抛out_of_range) {
    gw::RegisterTable t(8);
    const std::vector<std::uint16_t> three{1, 2, 3};
    EXPECT_THROW(t.read(8, false), std::out_of_range);
    EXPECT_THROW(t.write(8, 1), std::out_of_range);
    EXPECT_THROW(t.read_block(4, 8, false), std::out_of_range);
    EXPECT_THROW(t.write_block(6, three), std::out_of_range);
}

TEST(RegisterTable, 输入寄存器无写入通道且两空间互不串扰) {
    gw::RegisterTable t(4, 4);
    EXPECT_EQ(t.read(2, true), std::uint16_t{0});

    // write() 只作用于保持寄存器空间（地址 2 在两个空间都存在，正好用来证明不串扰）
    t.write(2, 1234);
    EXPECT_EQ(t.read(2, false), std::uint16_t{1234});  // 保持寄存器被改
    EXPECT_EQ(t.read(2, true), std::uint16_t{0});      // 输入寄存器不受影响
    EXPECT_EQ(t.write_count(), std::uint64_t{1});
}

TEST(RegisterTable, 块读写) {
    gw::RegisterTable t(16);
    const std::vector<std::uint16_t> values = {10, 20, 30, 40};
    t.write_block(4, values);
    EXPECT_EQ(t.read_block(4, 4, false), values);
    EXPECT_EQ(t.read(4, false), std::uint16_t{10});
    EXPECT_EQ(t.read(7, false), std::uint16_t{40});
    EXPECT_EQ(t.write_count(), std::uint64_t{4});
}

TEST(RegisterTable, 空块写入被拒) {
    gw::RegisterTable t(16);
    const std::vector<std::uint16_t> empty;
    EXPECT_THROW(t.write_block(0, empty), std::out_of_range);
}
