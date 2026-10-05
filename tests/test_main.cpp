#include "gtest_shim.h"

#if !defined(GW_HAVE_GTEST)

// 走垫片时由本文件提供 main
int main() {
    return ::gwshim::run_all();
}

#elif defined(GW_NEED_GTEST_MAIN)

// 系统里有 GoogleTest 但没有 gtest_main 时，自己提供 main
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#endif
